#!/usr/bin/env python3
"""Audit blind distributions; independently replay captured timed request logs."""
import collections
import csv
import math
from pathlib import Path
import sys
checks=0

def require(ok,message):
    global checks
    checks+=1
    if not ok:raise AssertionError(message)

def close(a,b,message):require(math.isclose(a,b,rel_tol=1e-8,abs_tol=2e-6),message)
def rows(path):
    with path.open() as f:return list(csv.DictReader(f))
def number(row,key):return float(row[key])
def count(row,key):return int(float(row[key]))
def bitcount(value):return bin(value).count('1')

def replay(prefix,metric,blocks,read_hist,interval_hist,attribution):
    events=rows(Path(str(prefix)+'.events.csv'))
    versions=collections.defaultdict(int)
    previous=-1
    for event in events:
        address=int(event['address']);time=number(event,'completed_ns')
        require(time>=previous,'completion log is not chronological');previous=time
        require(number(event,'issued_ns')<=number(event,'admitted_ns')<=time,'request violated timed dependencies')
        if event['write']=='1':versions[address]+=1
        require(int(event['version'])==versions[address],'read/write version inconsistent with completed stores')
    require(len(events)==count(metric,'accesses'),'event log lost requests')
    phases=rows(Path(str(prefix)+'.phases.csv'))
    by_admission=sorted(events,key=lambda e:int(e['admission_order']))
    for scope in ['all','measured']:
        selected=[e for e in by_admission if scope=='all' or e['measured']=='1']
        times=[p for p in phases if scope=='all' or p['measured']=='1']
        start=float(times[0]['start_ns']);end=float(times[-1]['end_ns'])
        for space in sorted({r['space'] for r in blocks}):
            base=1<<44;limit=base+count(metric,'table_lines')*64
            relevant=[e for e in selected if space=='all' or base<=int(e['address'])<limit]
            for size in [64,256,1024,4096]:
                key=lambda r:r['scope']==scope and r['space']==space and int(r['block_bytes'])==size
                output={int(r['address']):r for r in blocks if key(r)}
                observed=collections.defaultdict(list);misses=collections.defaultdict(lambda:[0,0])
                for e in relevant:observed[(int(e['address'])//size)*size].append(e)
                for e in selected:
                    targets=[(int(e['address']),int(e['demand_metadata_misses']),0)]
                    if e['victim_address']:targets.append((int(e['victim_address']),int(e['eviction_metadata_misses']),1))
                    for address,n,origin in targets:
                        if n and (space=='all' or base<=address<limit):misses[(address//size)*size][origin]+=n
                actual_reads=collections.Counter();actual_intervals=collections.Counter()
                for address,stream in observed.items():
                    row=output[address];read_lines={int(e['address'])//64 for e in stream if e['write']=='0'}
                    write_lines={int(e['address'])//64 for e in stream if e['write']=='1'}
                    require(count(row,'reads')==sum(e['write']=='0' for e in stream),'replay read count')
                    require(count(row,'writes')==sum(e['write']=='1' for e in stream),'replay write count')
                    require(count(row,'node_count')==len({int(e['node']) for e in stream}),'replay accessor union')
                    require(count(row,'written_lines')==len(write_lines) and count(row,'read_only_lines')==len(read_lines-write_lines),'replay spatial purity')
                    writes=[i for i,e in enumerate(stream) if e['write']=='1']
                    run=[]
                    if not writes:run.append(('both_censored',len(stream),end-start))
                    else:
                        run.append(('left_censored',writes[0],number(stream[writes[0]],'admitted_ns')-start))
                        for a,b in zip(writes,writes[1:]):run.append(('closed',b-a-1,number(stream[b],'admitted_ns')-number(stream[a],'admitted_ns')))
                        run.append(('right_censored',len(stream)-writes[-1]-1,end-number(stream[writes[-1]],'admitted_ns')))
                    for kind,length,duration in run:
                        actual_reads[kind,length]+=1
                        interval='zero' if duration==0 else str(math.floor(math.log2(duration)))
                        actual_intervals[kind,interval]+=1
                expected_reads=collections.Counter({(r['run_type'],int(r['consecutive_reads'])):int(r['runs']) for r in read_hist if key(r)})
                expected_intervals=collections.Counter({(r['run_type'],'zero' if r['bucket']=='zero' else str(round(math.log2(float(r['lower_ns']))))):int(r['runs']) for r in interval_hist if key(r)})
                require(actual_reads==expected_reads,'independent read-run replay differs')
                require(actual_intervals==expected_intervals,'independent interval replay differs')
                for address,row in output.items():
                    require(count(row,'demand_metadata_misses')==misses[address][0] and count(row,'eviction_metadata_misses')==misses[address][1],'miss address replay differs')
    return len(events)

def main():
    global checks
    root=Path(sys.argv[1]) if len(sys.argv)>1 else Path('results/blind')
    metrics=sorted(root.glob('*.metrics.csv'));require(bool(metrics),'no baseline metrics')
    replayed=0
    for path in metrics:
        prefix=Path(str(path)[:-len('.metrics.csv')]);metric=rows(path)[0]
        load=lambda suffix:rows(Path(str(prefix)+'.'+suffix+'.csv'))
        label=prefix.name
        require('scenario' not in metric and 'speedup' not in metric,'counterfactual output retained')
        close(number(metric,'cache_hits')+number(metric,'fills'),number(metric,'accesses'),label+': requests')
        close(number(metric,'reads')+number(metric,'writes'),number(metric,'accesses'),label+': operations')
        close(number(metric,'fill_bytes'),64*number(metric,'fills'),label+': fill bytes')
        close(number(metric,'writeback_bytes'),64*number(metric,'writebacks'),label+': writeback bytes')
        close(number(metric,'total_link_bytes'),sum(number(metric,k) for k in ['fill_bytes','writeback_bytes','control_bytes']),label+': link bytes')
        close(number(metric,'link_busy_ns'),number(metric,'total_link_bytes')/number(metric,'bandwidth_gbps'),label+': link bandwidth')
        require(number(metric,'link_busy_ns')<=1000*number(metric,'execution_us'),label+': link occupancy')
        require(count(metric,'peak_cache_lines')<=count(metric,'cache_bytes')//64,label+': data capacity')
        require(count(metric,'peak_inflight')<=(count(metric,'agents')+1)*count(metric,'outstanding'),label+': request window')
        require(number(metric,'aggregate_overlap_ns')<=number(metric,'aggregate_compute_ns'),label+': compute overlap')
        close(number(metric,'directory_requests'),number(metric,'directory_lookups')+number(metric,'evictions'),label+': eviction probes')
        for pre in ['','measured_']:
            close(number(metric,pre+'directory_requests'),sum(number(metric,pre+'directory_'+k) for k in ['hits','misses','merged']),label+': directory partition')
            close(number(metric,pre+'directory_read_bytes'),64*number(metric,pre+'directory_misses'),label+': metadata fetch bytes')
            close(number(metric,pre+'directory_write_bytes'),64*number(metric,pre+'directory_evictions'),label+': metadata replacement bytes')
            close(number(metric,pre+'directory_backing_busy_ns'),(number(metric,pre+'directory_read_bytes')+number(metric,pre+'directory_write_bytes'))/number(metric,'directory_backing_gbps'),label+': metadata bandwidth')
            require(count(metric,pre+'directory_peak_misses')<=count(metric,'directory_mshrs'),label+': miss buffers')
            require(count(metric,pre+'directory_peak_entries')<=count(metric,'directory_entries'),label+': metadata cache')
        phases=load('phases');close(float(phases[-1]['end_ns']),1000*number(metric,'execution_us'),label+': phase end')
        close(sum(float(p['duration_ns']) for p in phases if p['measured']=='1'),1000*number(metric,'measured_execution_us'),label+': measurement window')
        kinds=load('kinds')
        for scope in ['all','measured']:
            pre='' if scope=='all' else 'measured_';ks=[r for r in kinds if r['scope']==scope]
            for field in ['accesses','reads','writes','cache_hits','fills','directory_lookups','snoops','invalidations','writebacks','evictions','dirty_evictions']:
                close(sum(float(k[field]) for k in ks),number(metric,pre+field),label+': class '+pre+field)
        blocks=load('blocks');sharers=load('sharers');spatial=load('spatial');read_hist=load('read_runs');interval_hist=load('write_intervals');attrib=load('miss_attribution')
        for purity in spatial:
            key=lambda r:all(r[k]==purity[k] for k in ['scope','space','block_bytes'])
            bs=[b for b in blocks if key(b)];touched=[b for b in bs if count(b,'reads')+count(b,'writes')>0]
            require(len(touched)==count(purity,'observed_blocks'),label+': spatial denominator')
            categories=[sum(count(b,'writes')==0 for b in touched),sum(count(b,'writes')>0 and count(b,'read_only_lines')==0 for b in touched),sum(count(b,'writes')>0 and count(b,'read_only_lines')>0 for b in touched)]
            require(categories==[count(purity,k) for k in ['read_only_blocks','written_lines_only_blocks','mixed_blocks']],label+': purity partition')
            if purity['block_bytes']=='64':require(categories[2]==0,label+': 64B mixed spatial lines')
            for row in [s for s in sharers if key(s)]:require(count(row,'blocks')==sum(count(b,'node_count')==count(row,'node_count') for b in touched),label+': accessor histogram')
            expected_runs={'closed':sum(max(0,count(b,'writes')-1) for b in touched),'left_censored':sum(count(b,'writes')>0 for b in touched),'right_censored':sum(count(b,'writes')>0 for b in touched),'both_censored':sum(count(b,'writes')==0 for b in touched)}
            for kind,total in expected_runs.items():
                require(sum(count(r,'runs') for r in read_hist if key(r) and r['run_type']==kind)==total,label+': read run censoring')
                require(sum(count(r,'runs') for r in interval_hist if key(r) and r['run_type']==kind)==total,label+': interval censoring')
            for row in [a for a in attrib if key(a)]:
                writes='writes' if row['write_window']=='scope' else 'whole_run_writes'
                candidates=[b for b in bs if (count(b,writes)==0)==(row['write_class']=='zero_writes')]
                fields=['demand_metadata_misses'] if row['lookup_origin']=='demand' else ['eviction_metadata_misses'] if row['lookup_origin']=='eviction' else ['demand_metadata_misses','eviction_metadata_misses']
                expected=sum(sum(count(b,k) for k in fields) for b in candidates)
                require(count(row,'metadata_misses')==expected,label+': write/miss attribution')
            if purity['space']=='all':
                total_miss=sum(count(b,'demand_metadata_misses')+count(b,'eviction_metadata_misses') for b in bs)
                close(total_miss,number(metric,('measured_' if purity['scope']=='measured' else '')+'directory_misses'),label+': all-address miss accounting')
        if Path(str(prefix)+'.events.csv').exists():replayed+=replay(prefix,metric,blocks,read_hist,interval_hist,attrib)
    # Changing timing preserves the issued program addresses, not its coherence events.
    for mode in ['dlrm','radix']:
        a=root/('default.'+mode+'.metrics.csv');b=root/('slow_link.'+mode+'.metrics.csv')
        if a.exists() and b.exists():require(rows(a)[0]['trace_fingerprint']==rows(b)[0]['trace_fingerprint'],'hardware change changed program addresses')
    print(f'PASS: {checks} baseline/distribution checks across {len(metrics)} runs; independently replayed {replayed} timed requests.')

if __name__=='__main__':
    main()
