#!/usr/bin/env python3
"""Check fair inputs, transfer accounting, handoffs and replayed data freshness."""
import csv
import math
from pathlib import Path
import sys
import audit_perf as distribution_audit

checks = 0

def require(ok, message):
    global checks
    checks += 1
    if not ok:
        raise AssertionError(message)

def close(a, b, message):
    require(math.isclose(a, b, rel_tol=1e-8, abs_tol=2e-6), message)

def rows(path):
    with path.open() as stream:
        return list(csv.DictReader(stream))

def n(row, key):
    return float(row[key])


def audit_run(prefix, comparison):
    metric = rows(Path(str(prefix) + '.metrics.csv'))[0]
    load = lambda kind: rows(Path(str(prefix) + '.' + kind + '.csv'))
    label = prefix.name
    extra = comparison
    close(n(metric, 'measured_execution_us'), n(extra, 'measured_execution_us'), label + ': measured time')
    close(n(metric, 'cache_hits') + n(metric, 'fills'), n(metric, 'accesses'), label + ': hit/fill partition')
    close(n(metric, 'reads') + n(metric, 'writes'), n(metric, 'accesses'), label + ': read/write partition')
    close(n(metric, 'fill_bytes'), 64 * n(metric, 'fills'), label + ': data bytes')
    close(n(metric, 'writeback_bytes'), 64 * n(metric, 'writebacks') + n(extra, 'transition_data_bytes'), label + ': dirty bytes')
    controls = (n(metric, 'directory_lookups') * n(metric, 'directory_bytes') + 8 * n(metric, 'evictions') +
                n(metric, 'snoops') * n(metric, 'snoop_bytes') + n(metric, 'invalidations') * n(metric, 'invalidate_bytes'))
    close(n(metric, 'control_bytes'), controls + n(extra, 'software_control_bytes') + n(extra, 'transition_control_bytes'), label + ': control bytes')
    close(n(metric, 'total_link_bytes'), sum(n(metric, key) for key in ('fill_bytes', 'writeback_bytes', 'control_bytes')), label + ': total bytes')
    close(n(metric, 'link_busy_ns'), n(metric, 'total_link_bytes') / n(metric, 'bandwidth_gbps'), label + ': link budget')
    require(n(metric, 'link_busy_ns') <= 1000 * n(metric, 'execution_us'), label + ': impossible link occupancy')
    require(n(metric, 'peak_cache_lines') <= n(metric, 'cache_bytes') / 64, label + ': cache overflow')
    require(n(metric, 'peak_inflight') <= (n(metric, 'agents') + 1) * n(metric, 'outstanding'), label + ': MLP overflow')
    require(n(metric, 'directory_peak_misses') <= n(metric, 'directory_mshrs'), label + ': miss-buffer overflow')
    require(n(metric, 'directory_peak_entries') <= n(metric, 'directory_entries'), label + ': home metadata overflow')
    require(n(extra, 'peak_regions') <= n(extra, 'region_entries'), label + ': region table overflow')
    for pre in ('', 'measured_'):
        close(n(metric, pre + 'directory_requests'), n(metric, pre + 'directory_lookups') + n(metric, pre + 'evictions'), label + ': notices')
        close(n(metric, pre + 'directory_requests'), sum(n(extra, pre + k) for k in ('software_requests', 'hardware_requests')), label + ': route accounting')
        close(n(metric, pre + 'directory_requests'), sum(n(metric, pre + 'directory_' + k) for k in ('hits', 'misses', 'merged')), label + ': metadata partition')
        close(n(metric, pre + 'directory_read_bytes'), 64 * n(metric, pre + 'directory_misses'), label + ': metadata read bytes')
        close(n(metric, pre + 'directory_write_bytes'), 64 * n(metric, pre + 'directory_evictions'), label + ': metadata write bytes')
        close(n(extra, pre + 'software_busy_ns'), n(extra, pre + 'software_requests') * n(extra, 'software_ns'), label + ': runtime handler budget')
        close(n(extra, pre + 'transition_data_bytes'), 64 * n(extra, pre + 'flushed_lines'), label + ': transition flush bytes')
        close(n(extra, pre + 'transitions'), n(extra, pre + 'to_hardware') + n(extra, pre + 'to_software'), label + ': transition partition')
        if extra['mode'] == 'software':
            require(n(extra, pre + 'hardware_requests') == 0, label + ': noncoherent baseline used hardware tracking')
            close(n(extra, pre + 'software_control_bytes'), n(extra, 'software_message_bytes') *
                  (n(extra, pre + 'software_requests') + n(metric, pre + 'snoops') + n(metric, pre + 'invalidations')), label + ': software messages')
        if extra['mode'] == 'hardware':
            require(n(extra, pre + 'software_requests') == 0, label + ': hardware used software handlers')
        if extra['mode'] == 'dynamic':
            close(n(extra, pre + 'region_checks'), n(metric, pre + 'accesses'), label + ': retry double-counted region checks')
    require(n(extra, 'software_busy_ns') <= n(extra, 'software_workers') * 1000 * n(metric, 'execution_us'), label + ': software workers overcommitted')
    phases = load('phases')
    close(sum(n(p, 'duration_ns') for p in phases if p['measured'] == '1'), 1000 * n(metric, 'measured_execution_us'), label + ': warmup')
    changes = load('region_changes')
    last = {}
    for change in changes:
        address = int(change['address'])
        require(address % int(n(extra, 'region_bytes')) == 0, label + ': region alignment')
        require(change['from'] != change['to'] and change['to'] in ('software', 'hardware'), label + ': invalid transition')
        prior = last.get(address, ('software', 0.))
        require(change['from'] == prior[0] and n(change, 'start_ns') >= prior[1], label + ': overlapping/reordered handoffs')
        require(n(change, 'dirty_lines') <= n(change, 'resident_lines'), label + ': dirty lines without a copy')
        close(n(change, 'data_bytes'), 64 * n(change, 'dirty_lines'), label + ': handoff payload')
        close(n(change, 'control_bytes'), n(extra, 'transition_message_bytes') + n(extra, 'record_bytes') * n(change, 'resident_lines'), label + ': handoff records')
        minimum = (n(extra, 'transition_ns') + n(change, 'dirty_lines') * n(metric, 'writeback_ns') +
                   (n(metric, 'memory_ns') if n(change, 'dirty_lines') else 0) +
                   (n(change, 'data_bytes') + n(change, 'control_bytes')) / n(metric, 'bandwidth_gbps'))
        require(n(change, 'end_ns') - n(change, 'start_ns') + 1e-6 >= minimum, label + ': free handoff')
        last[address] = change['to'], n(change, 'end_ns')
    for pre in ('', 'measured_'):
        chosen = [r for r in changes if not pre or r['measured'] == '1']
        close(n(extra, pre + 'transitions'), len(chosen), label + ': transition log count')
        close(n(extra, pre + 'flushed_lines'), sum(n(r, 'dirty_lines') for r in chosen), label + ': flush log')
        close(n(extra, pre + 'transition_control_bytes'), sum(n(r, 'control_bytes') for r in chosen), label + ': transition controls')
        close(n(extra, pre + 'transition_service_ns'), sum(n(r, 'end_ns') - n(r, 'start_ns') for r in chosen), label + ': transition time')
    if extra['mode'] != 'dynamic':
        require(not changes and n(extra, 'region_checks') == 0, label + ': unexpected dynamic activity')
    replayed = 0
    if Path(str(prefix) + '.events.csv').exists():
        replayed = distribution_audit.replay(prefix, metric, load('blocks'), load('read_runs'), load('write_intervals'), load('miss_attribution'))
        events = load('events')
        size = int(n(extra, 'region_bytes'))
        by_region = {}
        for change in changes:
            by_region.setdefault(int(change['address']) // size, []).append(change)
        for event in events:
            keys = {int(event['address']) // size}
            if event['victim_address']:
                keys.add(int(event['victim_address']) // size)
            for key in keys:
                for change in by_region.get(key, []):
                    require(not (n(event, 'admitted_ns') < n(change, 'end_ns') and n(event, 'completed_ns') > n(change, 'start_ns')),
                            label + ': access/victim overlapped a handoff')
    return metric, replayed


def main():
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path('results/management')
    comparisons = sorted(root.glob('*.comparison.csv'))
    require(bool(comparisons), 'no comparison outputs')
    replayed = 0
    hardware_inputs = ('seed', 'layout_seed', 'agents', 'rounds', 'warmup_rounds', 'lead_steps', 'prefix_reads',
                       'leaf_writes', 'metadata_ops', 'prefix_lines', 'leaf_lines', 'stores_per_line', 'cache_bytes',
                       'outstanding', 'compute_ns', 'host_compute_ns', 'overlap_trigger', 'hit_ns', 'memory_ns', 'peer_ns',
                       'bandwidth_gbps', 'directory_ns', 'snoop_ns', 'invalidate_ns', 'writeback_ns', 'directory_bytes',
                       'snoop_bytes', 'invalidate_bytes', 'directory_entries', 'directory_banks', 'directory_mshrs',
                       'directory_issue_ns', 'directory_miss_ns', 'directory_backing_gbps', 'table_lines', 'sparse_reads',
                       'scratch_lines', 'scratch_reads', 'scratch_writes', 'hot_fraction', 'hot_read_fraction', 'zipf_alpha',
                       'bottom_fraction', 'hot_update_every', 'hot_updates', 'cold_update_every', 'cold_updates')
    for path in comparisons:
        group = rows(path)
        require({r['mode'] for r in group} == {'software', 'hardware', 'dynamic'}, path.name + ': missing modes')
        reference = next(r for r in group if r['mode'] == 'software')
        manager_inputs = ('region_bytes', 'region_entries', 'review_accesses', 'promote_operations',
                          'software_ns', 'software_workers', 'software_message_bytes', 'region_lookup_ns',
                          'transition_ns', 'transition_message_bytes', 'record_bytes')
        for comparison in group[1:]:
            for key in manager_inputs:
                require(comparison[key] == group[0][key], path.name + ': unequal management assumptions: ' + key)
        baseline = n(reference, 'measured_execution_us')
        metrics = []
        for comparison in group:
            close(n(comparison, 'software_baseline_us'), baseline, path.name + ': baseline mismatch')
            close(n(comparison, 'time_reduction_pct'), 100 * (baseline - n(comparison, 'measured_execution_us')) / baseline, path.name + ': reduction arithmetic')
            close(n(comparison, 'speedup'), baseline / n(comparison, 'measured_execution_us'), path.name + ': speedup arithmetic')
            prefix = Path(str(path)[:-len('.comparison.csv')] + '.' + comparison['mode'])
            metric, replay_count = audit_run(prefix, comparison)
            metrics.append(metric)
            replayed += replay_count
        for metric in metrics[1:]:
            for key in hardware_inputs + ('trace_fingerprint', 'accesses', 'reads', 'writes', 'measured_accesses', 'measured_reads', 'measured_writes'):
                require(metric[key] == metrics[0][key], path.name + ': unequal workload/hardware: ' + key)
        if path.name.startswith('software_free.'):
            close(n(group[0], 'measured_execution_us'), n(next(r for r in group if r['mode'] == 'hardware'), 'measured_execution_us'), 'free-software control is not the same baseline')
    print(f'PASS: {checks + distribution_audit.checks} accounting/freshness checks across {len(comparisons)} comparisons; '
          f'independently replayed {replayed} requests.')


if __name__ == '__main__':
    main()
