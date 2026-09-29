#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Summarize every profile in the same-guest Linux/core read comparison."""
import argparse
import json
import statistics
from collections import defaultdict
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('console', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    text = args.console.read_text()
    if 'LINUX_READ_RESULT=PASS' not in text or 'LINUX_READ_RESULT=FAIL' in text:
        raise RuntimeError('guest did not complete all reads and validation')
    rows = [json.loads(line.removeprefix('READ_SAMPLE ')) for line in text.splitlines()
            if line.startswith('READ_SAMPLE ')]
    groups = defaultdict(lambda: defaultdict(dict))
    for row in rows:
        key = (row['file'], row['cache'], row['access'])
        bucket = groups[key][row['reader']]
        if row['sample'] in bucket or row['elapsed_ns'] <= 0 or row['bytes'] <= 0:
            raise RuntimeError('duplicate or invalid measurement')
        bucket[row['sample']] = row
    expected_keys = {(file, cache, access) for file in ('contiguous.bin', 'sparse.bin')
                     for cache in ('warm', 'guest-cold') for access in ('sequential', 'random')}
    if set(groups) != expected_keys:
        raise RuntimeError('missing or unexpected benchmark profiles')
    report = dict(scope='Core library over buffered raw-device pread versus Linux ext4 VFS pread; '
                        'same guest, CPU, immutable image and logical requests; '
                        'guest-cold does not mean host/storage-cold; no native-adapter claim',
                  samples=rows, profiles=[])
    for key in sorted(groups):
        contenders = groups[key]
        if set(contenders) not in ({'core', 'linux'}, {'core', 'linux', 'raw'}):
            raise RuntimeError('missing contender')
        if 'raw' in contenders and key[0] != 'contiguous.bin':
            raise RuntimeError('raw diagnostic requires a contiguous file')
        core, linux = contenders['core'], contenders['linux']
        if len(core) < 7 or set(core) != set(linux) or set(core) != set(range(len(core))):
            raise RuntimeError('incomplete paired samples')
        pairs = []
        result = dict(zip(('file', 'cache', 'access'), key))
        for sample in sorted(core):
            if any(set(values) != set(core) for values in contenders.values()):
                raise RuntimeError('incomplete diagnostic samples')
            if any(core[sample][field] != values[sample][field]
                   for values in contenders.values() for field in ('bytes', 'request_bytes')):
                raise RuntimeError('unequal work per paired sample')
            pairs.append(linux[sample]['elapsed_ns'] / core[sample]['elapsed_ns'])
        for label, values in contenders.items():
            times = sorted(row['elapsed_ns'] for row in values.values())
            amount = {row['bytes'] for row in values.values()}
            if len(amount) != 1:
                raise RuntimeError('unequal work between samples')
            median = statistics.median(times)
            result[label] = dict(samples=len(times), bytes=amount.pop(), median_ns=median,
                                 p95_ns=times[(95 * len(times) + 99) // 100 - 1],
                                 min_ns=min(times), max_ns=max(times),
                                 cpu_median_ns=statistics.median(row['cpu_ns'] for row in values.values()),
                                 device_reads=[row['device_reads'] for row in values.values()],
                                 device_read_bytes=[row['device_read_bytes'] for row in values.values()])
            result[label]['mib_per_second'] = result[label]['bytes'] / (1024 * 1024) / (median / 1e9)
        result['throughput_ratio_core_to_linux'] = result['linux']['median_ns'] / result['core']['median_ns']
        result['paired_throughput_ratios'] = pairs
        result['all_pairs_at_least_15_percent_faster'] = min(pairs) >= 1.15
        if 'raw' in result:
            result['raw_diagnostic_ratio_to_linux'] = result['linux']['median_ns'] / result['raw']['median_ns']
        report['profiles'].append(result)
        print(f"{key}: Linux {result['linux']['mib_per_second']:.1f} MiB/s; "
              f"core {result['core']['mib_per_second']:.1f} MiB/s; "
              f"ratio {result['throughput_ratio_core_to_linux']:.3f}; "
              f"paired range {min(pairs):.3f}..{max(pairs):.3f}")
        if 'raw' in result:
            print(f"  Raw backend only (not core): {result['raw_diagnostic_ratio_to_linux']:.3f}x Linux")
    args.output.write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
