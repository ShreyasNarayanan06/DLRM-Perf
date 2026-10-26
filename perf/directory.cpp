#include "directory.hpp"
#include <algorithm>
#include <limits>

namespace perf {
DirectoryStats& DirectoryStats::operator+=(const DirectoryStats& r) {
    requests+=r.requests; hits+=r.hits; misses+=r.misses; merged+=r.merged; evictions+=r.evictions;
    read_bytes+=r.read_bytes; write_bytes+=r.write_bytes;
    bank_wait_ns+=r.bank_wait_ns; mshr_wait_ns+=r.mshr_wait_ns; capacity_wait_ns+=r.capacity_wait_ns;
    bank_busy_ns+=r.bank_busy_ns; backing_busy_ns+=r.backing_busy_ns; backing_wait_ns+=r.backing_wait_ns;
    service_ns+=r.service_ns;
    peak_misses=std::max(peak_misses,r.peak_misses); peak_entries=std::max(peak_entries,r.peak_entries);
    return *this;
}
DirectoryStats DirectoryStats::operator-(const DirectoryStats& r) const {
    auto d=*this;
    d.requests-=r.requests; d.hits-=r.hits; d.misses-=r.misses; d.merged-=r.merged; d.evictions-=r.evictions;
    d.read_bytes-=r.read_bytes; d.write_bytes-=r.write_bytes;
    d.bank_wait_ns-=r.bank_wait_ns; d.mshr_wait_ns-=r.mshr_wait_ns; d.capacity_wait_ns-=r.capacity_wait_ns;
    d.bank_busy_ns-=r.bank_busy_ns; d.backing_busy_ns-=r.backing_busy_ns; d.backing_wait_ns-=r.backing_wait_ns;
    d.service_ns-=r.service_ns;
    // Peaks describe the whole run, not a subtractable count.
    return d;
}
DirectoryModel::DirectoryModel(const Config& c) : c_(c), banks_(c.directory_banks,0) {}
double DirectoryModel::reserve(double arrival, std::uint64_t key, double weight, AccessKind kind) {
    auto& stats=stats_[index(kind)]; ++stats.requests;
    if (!c_.directory_model) return arrival;
    // Mix both namespaces without sending all even line keys to half the banks.
    auto hash=key+0x9e3779b97f4a7c15ULL;
    hash=(hash^(hash>>30))*0xbf58476d1ce4e5b9ULL;
    hash=(hash^(hash>>27))*0x94d049bb133111ebULL; hash^=hash>>31;
    auto& available=banks_[hash % banks_.size()];
    const auto start=std::max(arrival,available);
    const auto busy=c_.directory_issue_ns*weight;
    available=start+busy;
    stats.bank_wait_ns+=start-arrival; stats.bank_busy_ns+=busy;
    return start;
}
DirectoryModel::Lookup DirectoryModel::lookup(double now, std::uint64_t key, double weight, AccessKind kind) {
    auto& stats=stats_[index(kind)];
    const auto hit_service=c_.directory_ns*weight;
    if (!c_.directory_model) {
        ++stats.hits; stats.service_ns+=hit_service;
        return {false,now+hit_service};
    }
    auto found=entries_.find(key);
    if (found!=entries_.end()) {
        const auto ready=found->second.ready;
        lru_.splice(lru_.begin(),lru_,found->second.lru);
        if (ready>now) ++stats.merged; else ++stats.hits;
        const auto done=std::max(now+hit_service,ready);
        stats.service_ns+=done-now;
        return {false,done};
    }
    while (!misses_.empty() && misses_.top()<=now) misses_.pop();
    if (misses_.size()==c_.directory_mshrs) {
        stats.mshr_wait_ns+=misses_.top()-now;
        return {true,misses_.top()};
    }
    // A tag with a pending fill cannot be evicted or read as an immediate hit.
    auto victim=lru_.end();
    if (c_.directory_entries!=0 && entries_.size()==c_.directory_entries) {
        auto earliest=std::numeric_limits<double>::infinity();
        for (auto it=lru_.rbegin();it!=lru_.rend();++it) {
            const auto ready=entries_.at(*it).ready;
            earliest=std::min(earliest,ready);
            if (ready<=now) { victim=std::prev(it.base()); break; }
        }
        if (victim==lru_.end()) {
            stats.capacity_wait_ns+=earliest-now;
            return {true,earliest};
        }
    }
    std::uint64_t bytes=64; // One metadata cache line fetched from home backing memory.
    if (victim!=lru_.end()) {
        // Permission lookups update owner/sharer state; displaced metadata is dirty.
        ++stats.evictions; stats.write_bytes+=64; bytes+=64;
        entries_.erase(*victim); lru_.erase(victim);
    }
    ++stats.misses; stats.read_bytes+=64;
    const auto start=std::max(now,backing_available_);
    const auto busy=static_cast<double>(bytes)*weight/c_.directory_backing_gbps;
    backing_available_=start+busy;
    const auto done=backing_available_+hit_service+c_.directory_miss_ns*weight;
    stats.backing_wait_ns+=start-now; stats.backing_busy_ns+=busy; stats.service_ns+=done-now;
    misses_.push(done);
    if (c_.directory_entries!=0) {
        lru_.push_front(key); entries_.emplace(key,Entry{done,lru_.begin()});
    }
    stats.peak_misses=std::max(stats.peak_misses,misses_.size());
    stats.peak_entries=std::max(stats.peak_entries,entries_.size());
    return {false,done};
}
} // namespace perf
