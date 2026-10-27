#include "management.hpp"
#include "histograms.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace perf {
const char* name(ManagementMode mode) {
    switch(mode) {
    case ManagementMode::Software:return "software";
    case ManagementMode::Hardware:return "hardware";
    case ManagementMode::Dynamic:return "dynamic";
    }
    throw std::logic_error("invalid management mode");
}
void ManagementConfig::validate() const {
    if(region_bytes<64 || region_bytes>4096 || (region_bytes&(region_bytes-1))!=0 ||
       region_entries==0 || review_accesses==0 || promote_operations==0 || software_workers==0)
        throw std::invalid_argument("region bytes must be a power of two in 64..4096; capacities/thresholds must be positive");
    for(auto value:{software_ns,software_message_bytes,region_lookup_ns,transition_ns,transition_message_bytes,record_bytes})
        if(!std::isfinite(value) || value<0)throw std::invalid_argument("management costs must be finite and nonnegative");
}
ManagementModel::ManagementModel(const Config& hardware,const ManagementConfig& config):
    hardware_(hardware),config_(config),workers_(config.software_workers,0) {config_.validate();}
void ManagementModel::count(bool measured,void (*action)(ManagementStats&,double),double value) {
    action(all_,value);if(measured)action(measured_,value);
}
ManagementModel::Region* ManagementModel::ensure(std::uint64_t key) {
    auto found=regions_.find(key);if(found!=regions_.end())return &found->second;
    if(regions_.size()==config_.region_entries) {
        auto victim=regions_.end();
        for(auto it=regions_.begin();it!=regions_.end();++it) {
            const auto& r=it->second;
            if(r.mode!=ManagementMode::Software || r.changing || r.draining || active_.count(it->first))continue;
            if(victim==regions_.end() || r.last_touch<victim->second.last_touch)victim=it;
        }
        if(victim==regions_.end())return nullptr; // Safely use software if no tracking slot is available.
        regions_.erase(victim);
    }
    auto& region=regions_[key];region.last_touch=++clock_;
    all_.peak_regions=std::max(all_.peak_regions,regions_.size());return &region;
}
ManagementMode ManagementModel::mode_at(std::uint64_t address) const {
    if(config_.mode!=ManagementMode::Dynamic)return config_.mode;
    const auto found=regions_.find(address/config_.region_bytes);
    return found==regions_.end() ? ManagementMode::Software : found->second.mode;
}
ManagementModel::Gate ManagementModel::gate(std::uint64_t address) {
    if(config_.mode!=ManagementMode::Dynamic)return Gate::Ready;
    const auto key=address/config_.region_bytes;auto* region=ensure(key);
    if(region==nullptr)return Gate::Ready;
    if(region->changing)return Gate::Blocked;
    if(region->desired==region->mode)return Gate::Ready;
    region->draining=true;
    return active_.count(key)!=0 ? Gate::Blocked : Gate::Change;
}
bool ManagementModel::victim_allowed(std::uint64_t address) const {
    if(config_.mode!=ManagementMode::Dynamic)return true;
    const auto found=regions_.find(address/config_.region_bytes);
    return found==regions_.end() || (!found->second.changing && !found->second.draining);
}
RegionChange ManagementModel::begin_change(std::uint64_t address,double time,bool measured,const BaselineMemory& memory) {
    const auto key=address/config_.region_bytes;auto& region=regions_.at(key);
    if(active_.count(key) || region.changing || region.mode==region.desired)throw std::logic_error("unsafe region handoff");
    const auto resident=memory.region_contents(key,config_.region_bytes);
    RegionChange change{key,region.mode,region.desired,time,0,resident.second,resident.first,measured};
    region.changing=true;pending_changes_[key]=changes_.size();changes_.push_back(change);return change;
}
void ManagementModel::finish_change(std::uint64_t address,double time,BaselineMemory& memory) {
    const auto key=address/config_.region_bytes;auto& change=changes_.at(pending_changes_.at(key));
    if(active_.count(key))throw std::logic_error("region changed with live accesses");
    if(memory.flush_region(key,config_.region_bytes)!=change.dirty_lines)throw std::logic_error("handoff dirty data changed while draining");
    auto& region=regions_.at(key);region.mode=change.to;region.changing=false;region.draining=false;change.end_ns=time;
    for(auto* stats:{&all_,change.measured ? &measured_ : nullptr})if(stats) {
        ++stats->transitions;
        if(change.to==ManagementMode::Hardware)++stats->to_hardware;else ++stats->to_software;
        stats->flushed_lines+=change.dirty_lines;
        stats->transition_control_bytes+=config_.transition_message_bytes+config_.record_bytes*static_cast<double>(change.resident_lines);
        stats->transition_data_bytes+=64*static_cast<double>(change.dirty_lines);
        stats->transition_service_ns+=time-change.start_ns;
    }
    pending_changes_.erase(key);
}
void ManagementModel::admitted(const Transaction& tx) {
    const auto key=tx.access.address/config_.region_bytes;
    ++active_[key];if(tx.has_victim && tx.victim/config_.region_bytes!=key)++active_[tx.victim/config_.region_bytes];
    if(config_.mode!=ManagementMode::Dynamic)return;
    count(tx.measured,[](ManagementStats& s,double){++s.region_checks;},0);
    auto found=regions_.find(key);
    if(found==regions_.end()) {count(tx.measured,[](ManagementStats& s,double){++s.table_pressure;},0);return;}
    auto& region=found->second;region.last_touch=++clock_;
    region.nodes|=1ULL<<tx.node;++region.observations;region.operations+=static_cast<std::size_t>(tx.traffic.directory);
    if(region.observations>=config_.review_accesses) {
        if(region.mode==ManagementMode::Software && bit_count(region.nodes)>1 && region.operations>=config_.promote_operations)
            region.desired=ManagementMode::Hardware;
        else if(region.mode==ManagementMode::Hardware && region.operations==0)region.desired=ManagementMode::Software;
        region.nodes=0;region.observations=0;region.operations=0;
    }
    measured_.peak_regions=all_.peak_regions; // Peak table occupancy is run-wide.
}
void ManagementModel::completed(const Transaction& tx) {
    const auto key=tx.access.address/config_.region_bytes;
    const auto drop=[&](std::uint64_t region) {
        auto found=active_.find(region);
        if(found==active_.end() || found->second==0)throw std::logic_error("missing active region reservation");
        if(--found->second==0)active_.erase(found);
    };
    drop(key);if(tx.has_victim && tx.victim/config_.region_bytes!=key)drop(tx.victim/config_.region_bytes);
}
double ManagementModel::request_bytes(const Transaction& tx) {
    double bytes=0;for(const auto& op:tx.operations)if(mode_at(op.address)==ManagementMode::Software)bytes+=config_.software_message_bytes;
    count(tx.measured,[](ManagementStats& s,double b){s.software_control_bytes+=b;},bytes);return bytes;
}
double ManagementModel::response_bytes(const Transaction& tx) {
    const auto bytes=mode_at(tx.access.address)==ManagementMode::Software ?
        static_cast<double>(tx.traffic.snoops+tx.traffic.invalidations)*config_.software_message_bytes : 0;
    count(tx.measured,[](ManagementStats& s,double b){s.software_control_bytes+=b;},bytes);return bytes;
}
double ManagementModel::operation_ready(double time,std::uint64_t address,bool measured) {
    if(mode_at(address)==ManagementMode::Hardware) {
        count(measured,[](ManagementStats& s,double){++s.hardware_requests;},0);return time;
    }
    auto worker=std::min_element(workers_.begin(),workers_.end());const auto start=std::max(time,*worker);
    *worker=start+config_.software_ns;
    count(measured,[](ManagementStats& s,double){++s.software_requests;},0);
    count(measured,[](ManagementStats& s,double n){s.software_queue_ns+=n;},start-time);
    count(measured,[](ManagementStats& s,double n){s.software_busy_ns+=n;},config_.software_ns);
    return *worker;
}
double ManagementModel::check_ns() const {return config_.mode==ManagementMode::Dynamic ? config_.region_lookup_ns : 0;}
double ManagementModel::change_bytes(const RegionChange& change) const {
    return config_.transition_message_bytes+config_.record_bytes*static_cast<double>(change.resident_lines)+64*static_cast<double>(change.dirty_lines);
}
double ManagementModel::change_service(const RegionChange& change) const {
    return config_.transition_ns+static_cast<double>(change.dirty_lines)*hardware_.writeback_ns+
        (change.dirty_lines==0 ? 0 : hardware_.memory_ns);
}
} // namespace perf
