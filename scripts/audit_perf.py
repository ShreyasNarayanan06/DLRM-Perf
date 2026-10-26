#!/usr/bin/env python3
"""Audit CSV exports from either revision; uses only Python's standard library."""
import csv
import math
from pathlib import Path
import sys
root=Path(sys.argv[1]) if len(sys.argv)>1 else Path('results/dlrm_refined')
cases=sorted(p.stem for p in root.glob('*.csv') if not p.stem.endswith(('_kinds','_phases')))
checks=0

def require(ok,message):
    global checks
    checks+=1
    if not ok: raise AssertionError(message)

def close(a,b,message): require(math.isclose(a,b,rel_tol=1e-8,abs_tol=1e-6),message)
def load(case,suffix=''):
    with (root/(case+suffix+'.csv')).open() as f:return list(csv.DictReader(f))

hardware=['agents','cache_bytes','outstanding','compute_ns','host_compute_ns','hit_ns','memory_ns','peer_ns',
          'bandwidth_gbps','directory_ns','snoop_ns','invalidate_ns','writeback_ns','directory_bytes','snoop_bytes',
          'invalidate_bytes','overlap_trigger','region_bytes','region_barrier_ns','fork_barrier_ns','setup_ns',
          'recovery_ns','recovery_bytes','recovery_rate','added_latency_ns','directory_model','directory_entries',
          'directory_banks','directory_mshrs','directory_issue_ns','directory_miss_ns','directory_backing_gbps']
traffic=['logical_accesses','reads','writes','cache_hits','fills','directory_lookups','snoops','invalidations',
         'writebacks','evictions','dirty_evictions','region_grants','region_barriers','region_recoveries',
         'region_downgrades','revoked_copies','collateral_copies','region_flushes']
dir_counts=['requests','hits','misses','merged','evictions','read_bytes','write_bytes']
require(bool(cases),'no result CSVs')
all_rows={}
for case in cases:
    rows=load(case);all_rows[case]=rows;kinds=load(case,'_kinds');phases=load(case,'_phases')
    modes=sorted({r['workload'] for r in rows})
    require(len(rows)==4*len(modes),case+': four scenarios required per workload')
    for mode in modes:
        group=[r for r in rows if r['workload']==mode]
        require(len({r['trace_fingerprint'] for r in group})==1,case+': scenario changed trace')
        baseline=float(group[0]['execution_us'])
        measured_baseline=float(group[0].get('measured_execution_us',baseline))
        for r in group:
            n=lambda k:float(r[k]);label=case+'/'+mode+'/'+r['scenario']
            require(all(math.isfinite(float(v)) for k,v in r.items() if k not in ('scenario','workload')),label+': nonfinite')
            close(n('cache_hits')+n('fills'),n('logical_accesses'),label+': request partition')
            close(n('reads')+n('writes'),n('logical_accesses'),label+': operations')
            close(n('fill_bytes'),64*n('fills'),label+': fills')
            close(n('writeback_bytes'),64*(n('writebacks')+n('fork_flushes')),label+': writebacks')
            close(n('total_link_bytes'),n('fill_bytes')+n('writeback_bytes')+n('control_bytes'),label+': bytes')
            close(n('speedup'),baseline/n('execution_us'),label+': speedup')
            close(n('execution_reduction_pct'),100*(baseline-n('execution_us'))/baseline,label+': reduction')
            close(n('link_utilization_pct'),100*n('total_link_bytes')/n('bandwidth_gbps')/(1000*n('execution_us')),label+': link utilization')
            require(n('link_utilization_pct')<=100.000001,label+': impossible link usage')
            require(n('aggregate_overlap_ns')<=n('aggregate_worker_compute_ns')+1e-6,label+': overlap exceeds compute')
            require(n('peak_inflight')<=(n('agents')+1)*n('outstanding'),label+': MLP exceeded')
            matching=[k for k in kinds if k['workload']==mode and k['scenario']==r['scenario']]
            region_kinds=[k for k in matching if k.get('scope','all')=='all']
            for key in traffic:close(sum(float(k[key]) for k in region_kinds),n(key),label+': class sum '+key)
            if r['scenario']=='Baseline Always-Coherent':
                for k in region_kinds:require(float(k['writebacks'])<=float(k['writes']),label+': victim attribution')
            timeline=[p for p in phases if p['workload']==mode and p['scenario']==r['scenario']]
            close(float(timeline[-1]['end_ns']),1000*n('execution_us'),label+': final timestamp')
            for a,b in zip(timeline,timeline[1:]):close(float(a['end_ns']),float(b['start_ns']),label+': phase gap')
            if 'measured_execution_us' not in r:continue
            close(n('measured_execution_reduction_pct'),100*(measured_baseline-n('measured_execution_us'))/measured_baseline,label+': measured reduction')
            close(n('measured_execution_us')*1000,sum(float(p['duration_ns']) for p in timeline if p['measured']=='1'),label+': measured time')
            measured=[k for k in matching if k['scope']=='measured']
            for key in ['logical_accesses','cache_hits','evictions']:
                close(sum(float(k[key]) for k in measured),n('measured_'+key),label+': measured '+key)
            for prefix,selected in [('directory_',region_kinds),('measured_directory_',measured)]:
                for key in dir_counts:close(sum(float(k['directory_'+key]) for k in selected),n(prefix+key),label+': '+prefix+key)
                close(n(prefix+'requests'),n(prefix+'hits')+n(prefix+'misses')+n(prefix+'merged'),label+': metadata requests')
                close(n(prefix+'read_bytes'),64*n(prefix+'misses'),label+': metadata fetch bytes')
                close(n(prefix+'write_bytes'),64*n(prefix+'evictions'),label+': metadata writeback bytes')
                require(n(prefix+'peak_misses')<=n('directory_mshrs'),label+': metadata MSHRs exceeded')
                require(n(prefix+'peak_entries')<=n('directory_entries'),label+': metadata capacity exceeded')
                weighted_bytes=0
                for k in selected:
                    weight=.5 if r['scenario']=='50% KV/Embedding Overhead Reduction' and k['kind']!='metadata' else 1
                    weighted_bytes+=(float(k['directory_read_bytes'])+float(k['directory_write_bytes']))*weight
                close(n(prefix+'backing_busy_ns'),weighted_bytes/n('directory_backing_gbps'),label+': metadata bandwidth budget')
    require(all(all(r[key]==rows[0][key] for key in hardware if key in r) for r in rows),case+': hardware differs between workloads/scenarios')
if 'dlrm_line_regions' in all_rows:
    coarse=next(r for r in all_rows['dlrm_line_regions'] if r['scenario']=='Naive Coarse Region Coherence')
    require(float(coarse['collateral_copies'])==0,'64B regions cause collateral sharing')
if 'long_seed43' in all_rows:
    require(all_rows['long_default'][-1]['trace_fingerprint']!=all_rows['long_seed43'][-1]['trace_fingerprint'],'seed did not change trace')
print(f'PASS: {checks} accounting, capacity, queue, warm-up, trace and hardware checks across {len(cases)} cases.')
