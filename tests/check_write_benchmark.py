#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Check and summarize matched durable-overwrite measurements; separate from reads."""
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
    if 'LINUX_WRITE_RESULT=PASS' not in text or 'LINUX_WRITE_RESULT=FAIL' in text:
        raise RuntimeError('guest did not complete writes and independent Linux validation')
    kernel = next((line.removeprefix('LINUX_WRITE_KERNEL=') for line in text.splitlines()
                   if line.startswith('LINUX_WRITE_KERNEL=')), None)
    if kernel is None:
        raise RuntimeError('missing actual guest kernel identity')
    rows = [json.loads(line.removeprefix('WRITE_SAMPLE ')) for line in text.splitlines()
            if line.startswith('WRITE_SAMPLE ')]
    groups = defaultdict(lambda: defaultdict(dict))
    counters = ('read_callbacks', 'read_bytes', 'write_callbacks', 'write_bytes', 'flushes',
                'core_allocations', 'core_peak_live_bytes', 'device_reads',
                'device_read_sectors', 'device_writes', 'device_write_sectors', 'device_flushes')
    for row in rows:
        bucket = groups[row['access']][row['writer']]
        if (row['sample'] in bucket or row['elapsed_ns'] <= 0 or row['cpu_ns'] <= 0 or
                any(not isinstance(row[field], int) or row[field] < 0 for field in counters)):
            raise RuntimeError('duplicate or invalid measurement')
        bucket[row['sample']] = row
    if set(groups) != {'sequential', 'random'}:
        raise RuntimeError('missing or unexpected write profiles')
    report = dict(
        scope='Core library over an exclusive buffered raw device versus Linux ext4 VFS; '
              'same guest and identical initial volumes, ordered data, preallocated warm '
              'overwrites, durability every 1 MiB included in timing; no native-adapter claim',
        kernel=kernel, samples=rows, profiles=[],
        validation='Every sample checked against expected bytes; Linux independently read '
                   'the final core output. Host nonrepairing fsck must be checked separately.',
        accounting='Core callback/allocation counters cover the core only. Linux zeros in '
                   'these fields do not imply zero allocation or I/O. Device sectors are '
                   '512-byte units; delayed checkpoints can extend beyond a sample.')
    for access in ('sequential', 'random'):
        contenders = groups[access]
        if set(contenders) != {'core', 'linux'} or any(
                set(values) != set(range(7)) for values in contenders.values()):
            raise RuntimeError('expected seven complete Linux/core pairs')
        request = 65536 if access == 'sequential' else 4096
        for values in contenders.values():
            if any(row['request_bytes'] != request or row['bytes'] != 256 * 1024 * 1024 or
                   row['sync_bytes'] != 1024 * 1024 for row in values.values()):
                raise RuntimeError('unexpected work or durability frequency')
        result = dict(access=access)
        for name, values in contenders.items():
            times = sorted(row['elapsed_ns'] for row in values.values())
            median = statistics.median(times)
            result[name] = dict(
                samples=len(times), median_ns=median, min_ns=min(times), max_ns=max(times),
                p95_ns=times[(95 * len(times) + 99) // 100 - 1],
                mib_per_second=256 / (median / 1e9),
                cpu_median_ns=statistics.median(row['cpu_ns'] for row in values.values()),
                counters={field: [values[index][field] for index in sorted(values)]
                          for field in counters},
                device_write_amplification=[values[index]['device_write_sectors'] * 512 /
                                            values[index]['bytes'] for index in sorted(values)])
        pairs = [contenders['linux'][index]['elapsed_ns'] /
                 contenders['core'][index]['elapsed_ns'] for index in range(7)]
        result['throughput_ratio_core_to_linux'] = (
            result['linux']['median_ns'] / result['core']['median_ns'])
        result['paired_throughput_ratios'] = pairs
        report['profiles'].append(result)
        print(f"{access}: Linux {result['linux']['mib_per_second']:.1f} MiB/s; "
              f"core {result['core']['mib_per_second']:.1f} MiB/s; "
              f"ratio {result['throughput_ratio_core_to_linux']:.3f}; "
              f"paired range {min(pairs):.3f}..{max(pairs):.3f}")
    report['write_geometric_mean'] = statistics.geometric_mean(
        profile['throughput_ratio_core_to_linux'] for profile in report['profiles'])
    args.output.write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
