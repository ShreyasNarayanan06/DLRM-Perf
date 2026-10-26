// CPU calibration aid. These measurements do not establish CXL/GPU latencies.
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <numeric>
#include <new>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <pthread.h>
#include <sched.h>
#include <immintrin.h>

struct alignas(64) Line { std::uint64_t next; volatile std::uint64_t value; char padding[48]; };
static_assert(sizeof(Line)==64,"line size");
class Barrier {
    std::mutex mutex_; std::condition_variable cv_; unsigned count_=0, epoch_=0;
public:
    void wait() {
        std::unique_lock<std::mutex> lock(mutex_); const auto epoch=epoch_;
        if(++count_==4) {count_=0;++epoch_;cv_.notify_all();}
        else cv_.wait(lock,[&]{return epoch_!=epoch;});
    }
};
std::uint64_t walk(Line* lines,std::size_t count,bool write) {
    std::size_t at=0;std::uint64_t sum=0;
    for(std::size_t n=0;n<count;++n) {
        if(write) lines[at].value=lines[at].value+1;
        sum+=lines[at].value;at=static_cast<std::size_t>(lines[at].next);
    }
    return sum+at;
}
int main(int argc,char** argv) {
    try {
        const std::string mode=argc>1?argv[1]:"handoff";
        const auto bytes=argc>2?std::stoull(argv[2]):65536ULL;
        const auto reps=argc>3?std::stoull(argv[3]):5ULL;
        if(bytes<64||bytes%64!=0||reps==0||bytes>64*1024*1024ULL)
            throw std::invalid_argument("bytes must be a multiple of 64 up to 64 MiB; repetitions positive");
        if(mode!="warm"&&mode!="cold"&&mode!="handoff"&&mode!="private")
            throw std::invalid_argument("case must be warm, cold, handoff or private");
        cpu_set_t allowed;CPU_ZERO(&allowed);
        if(sched_getaffinity(0,sizeof(allowed),&allowed)!=0) throw std::runtime_error("cannot read CPU affinity");
        std::vector<int> cpus;
        for(int cpu=0;cpu<CPU_SETSIZE&&cpus.size()<4;++cpu) if(CPU_ISSET(cpu,&allowed)) cpus.push_back(cpu);
        if(cpus.size()<4) throw std::runtime_error("probe requires four allocated CPUs");
        const auto count=static_cast<std::size_t>(bytes/64);
        std::vector<std::size_t> permutation(count);std::iota(permutation.begin(),permutation.end(),0);
        std::mt19937 rng(17);std::shuffle(permutation.begin(),permutation.end(),rng);
        const auto allocate=[&]() {
            void* storage=nullptr;if(posix_memalign(&storage,64,static_cast<std::size_t>(bytes))!=0) throw std::bad_alloc();
            auto* lines=static_cast<Line*>(storage);
            for(std::size_t n=0;n<count;++n) new(lines+n) Line;
            for(std::size_t n=0;n<count;++n){lines[permutation[n]].next=permutation[(n+1)%count];lines[n].value=1;}
            return lines;
        };
        Line* prefix=allocate();std::vector<Line*> private_lines;
        for(int n=0;n<4;++n) private_lines.push_back(allocate());
        struct Row {double elapsed=0;std::uint64_t sum=0;bool pinned=false;};
        std::vector<std::vector<Row>> rows(4,std::vector<Row>(static_cast<std::size_t>(reps)));
        Barrier barrier;std::vector<std::thread> threads;
        for(std::size_t tid=0;tid<4;++tid) threads.emplace_back([&,tid]{
            cpu_set_t set;CPU_ZERO(&set);CPU_SET(cpus[tid],&set);
            const bool pinned=pthread_setaffinity_np(pthread_self(),sizeof(set),&set)==0;
            for(std::size_t r=0;r<reps;++r) {
                if(tid==0) for(std::size_t n=0;n<count;++n) prefix[n].value=prefix[n].value+1;
                barrier.wait();
                std::uint64_t warm_sum=0;
                if(mode=="warm"&&tid!=0) warm_sum=walk(prefix,count,false);
                if(mode=="private") warm_sum=walk(private_lines[tid],count,true);
                barrier.wait();
                if(mode=="cold"&&tid==0) {
                    for(std::size_t n=0;n<count;++n) _mm_clflush(prefix+n);
                    _mm_mfence();
                }
                barrier.wait();
                if(tid!=0) {
                    const auto begin=std::chrono::steady_clock::now();
                    const auto sum=walk(mode=="private"?private_lines[tid]:prefix,count,mode=="private");
                    const auto end=std::chrono::steady_clock::now();
                    rows[tid][r]={std::chrono::duration<double,std::nano>(end-begin).count(),sum+warm_sum,pinned};
                }
                barrier.wait();
            }
        });
        for(auto& thread:threads) thread.join();
        std::cout<<"case,working_set_bytes,reader,cpu,affinity_ok,repetition,dependent_walk_ns_per_line,checksum\n";
        for(std::size_t tid=1;tid<4;++tid) for(std::size_t r=0;r<reps;++r)
            std::cout<<mode<<','<<bytes<<','<<tid<<','<<cpus[tid]<<','<<(rows[tid][r].pinned?1:0)<<','<<r<<','
                     <<rows[tid][r].elapsed/static_cast<double>(count)<<','<<rows[tid][r].sum<<'\n';
        std::free(prefix);for(auto* lines:private_lines)std::free(lines);
        return 0;
    } catch(const std::exception& e){std::cerr<<"probe error: "<<e.what()<<'\n';return 1;}
}
