#pragma once
#include "profile.hpp"
#include <unordered_map>

namespace perf {
enum class ManagementMode { Software, Hardware, Dynamic };
const char* name(ManagementMode mode);
struct ManagementConfig {
    ManagementMode mode=ManagementMode::Hardware;
    std::size_t region_bytes=4096,region_entries=256,review_accesses=64,promote_operations=8;
    double software_ns=100,software_message_bytes=16;
    std::size_t software_workers=4;
    double region_lookup_ns=2,transition_ns=200,transition_message_bytes=32,record_bytes=16;
    void validate() const;
};
struct ManagementStats {
    std::uint64_t software_requests=0,hardware_requests=0,region_checks=0,table_pressure=0;
    std::uint64_t transitions=0,to_hardware=0,to_software=0,flushed_lines=0;
    double software_busy_ns=0,software_queue_ns=0,software_control_bytes=0;
    double transition_control_bytes=0,transition_data_bytes=0,transition_service_ns=0;
    std::size_t peak_regions=0;
};
struct RegionChange {
    std::uint64_t region=0;
    ManagementMode from=ManagementMode::Software,to=ManagementMode::Hardware;
    double start_ns=0,end_ns=0;
    std::size_t dirty_lines=0,resident_lines=0;
    bool measured=true;
};
// Both paths use explicit permission/copy state. Software mode executes a runtime
// handler for home-table operations; hardware mode executes only the controller.
// This reference adapts the manager, not the application's consistency contract.
class ManagementModel {
public:
    enum class Gate { Ready,Blocked,Change };
    ManagementModel(const Config& hardware,const ManagementConfig& config);
    Gate gate(std::uint64_t address);
    bool victim_allowed(std::uint64_t address) const;
    RegionChange begin_change(std::uint64_t address,double time,bool measured,const BaselineMemory& memory);
    void finish_change(std::uint64_t address,double time,BaselineMemory& memory);
    void admitted(const Transaction& tx);
    void completed(const Transaction& tx);
    double request_bytes(const Transaction& tx);
    double response_bytes(const Transaction& tx);
    double operation_ready(double time,std::uint64_t address,bool measured);
    double check_ns() const;
    double change_bytes(const RegionChange& change) const;
    double change_service(const RegionChange& change) const;
    ManagementMode mode_at(std::uint64_t address) const;
    const ManagementConfig& config() const { return config_; }
    const ManagementStats& stats(bool measured=false) const { return measured ? measured_ : all_; }
    const std::vector<RegionChange>& changes() const { return changes_; }
private:
    struct Region {
        ManagementMode mode=ManagementMode::Software,desired=ManagementMode::Software;
        std::uint64_t nodes=0,last_touch=0;
        std::size_t observations=0,operations=0;
        bool changing=false,draining=false;
    };
    Region* ensure(std::uint64_t region);
    void count(bool measured,void (*action)(ManagementStats&,double),double value);
    const Config& hardware_;
    ManagementConfig config_;
    std::vector<double> workers_;
    std::unordered_map<std::uint64_t,Region> regions_;
    std::unordered_map<std::uint64_t,std::size_t> active_;
    std::vector<RegionChange> changes_;
    std::unordered_map<std::uint64_t,std::size_t> pending_changes_;
    std::uint64_t clock_=0;
    ManagementStats all_,measured_;
};
} // namespace perf
