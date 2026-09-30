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
    parser.add_argument('--require-target', action='store_true',
                        help='fail unless the eight-profile reading target is met')
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
    report['core_read_contract'] = next(
        (line.removeprefix('CORE_READ_API=') for line in text.splitlines()
         if line.startswith('CORE_READ_API=')),
        'ext4_read; mapping state retained only within each request')
    report['backend_diagnostics'] = next(
        (line.removeprefix('READ_BACKENDS=') for line in text.splitlines()
         if line.startswith('READ_BACKENDS=')), None)
    for key in sorted(groups):
        contenders = groups[key]
        baseline = {'core', 'linux'} | ({'raw'} if key[0] == 'contiguous.bin' else set())
        diagnostics = {'core-demand', 'core-mapped', 'linux-mapped'}
        if set(contenders) not in (baseline, baseline | diagnostics):
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
        result['backend_comparisons'] = []
        for candidate, reference in [('core-demand', 'linux'), ('core-mapped', 'linux'),
                                     ('core-mapped', 'linux-mapped')]:
            if candidate not in contenders:
                continue
            ratios = [contenders[reference][sample]['elapsed_ns'] /
                      contenders[candidate][sample]['elapsed_ns'] for sample in sorted(core)]
            comparison = dict(candidate=candidate, reference=reference,
                              median_throughput_ratio=result[reference]['median_ns'] /
                              result[candidate]['median_ns'], paired_ratios=ratios)
            result['backend_comparisons'].append(comparison)
        report['profiles'].append(result)
        print(f"{key}: Linux {result['linux']['mib_per_second']:.1f} MiB/s; "
              f"core {result['core']['mib_per_second']:.1f} MiB/s; "
              f"ratio {result['throughput_ratio_core_to_linux']:.3f}; "
              f"paired range {min(pairs):.3f}..{max(pairs):.3f}")
        if 'raw' in result:
            print(f"  Raw backend only (not core): {result['raw_diagnostic_ratio_to_linux']:.3f}x Linux")
        for comparison in result['backend_comparisons']:
            ratios = comparison['paired_ratios']
            print(f"  Diagnostic {comparison['candidate']}/{comparison['reference']}: "
                  f"{comparison['median_throughput_ratio']:.3f}; "
                  f"paired range {min(ratios):.3f}..{max(ratios):.3f}")
    ratios = [profile['throughput_ratio_core_to_linux'] for profile in report['profiles']]
    mean = statistics.geometric_mean(ratios)
    below_linux = [{field: profile[field] for field in ('file', 'cache', 'access')}
                   for profile in report['profiles']
                   if profile['throughput_ratio_core_to_linux'] < 1.0]
    report['read_target'] = dict(
        comparison='Ratio of median throughput in each of the eight fixed profiles; '
                   'paired ranges are reported separately, with no noise tolerance',
        geometric_mean=mean, required_geometric_mean=1.15,
        required_minimum_profile_ratio=1.0, profiles_below_linux=below_linux,
        met=mean >= 1.15 and not below_linux)
    print(f"Read target: mean {mean:.4f}x; {len(below_linux)} profiles below Linux; "
          f"{'MET' if report['read_target']['met'] else 'NOT MET'}")
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    if args.require_target and not report['read_target']['met']:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
