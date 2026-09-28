# SPDX-License-Identifier: BSD-3-Clause
# Every test belongs to exactly one CI suite. Selecting a fixture profile adds
# required cases; missing images fail tests instead of silently removing them.
paths = {}
foreach option : ['fixtures', 'allocation_fixtures', 'orphan_file_fixtures',
  'namespace_fixtures', 'xattr_fixtures', 'ea_inode_fixtures', 'inline_fixtures', 'inline_space_fixtures', 'cluster_fixtures', 'cluster_space_fixtures', 'xattr_space_fixtures', 'xattr_enable_fixtures', 'space_fixtures',
  'index_fixtures', 'index_collisions', 'index_capacity', 'group_checksum_fixtures', 'geometry_fixtures',
  'large_directory_fixtures', 'large_directory_edges', 'directory_links_fixture',
  'directory_index_64k_fixture', 'large_file_fixtures', 'large_volume_fixtures', 'external_journal_fixtures',
  'fast_commit_fixtures']
  value = get_option(option)
  paths += {option: value == '' ? '' : join_paths(meson.project_source_root(), value)}
endforeach

if paths['fast_commit_fixtures'] != ''
  foreach profile : ['1k', '4k', 'checksum-seed', 'no-checksum', 'orphan-1k', 'orphan-4k']
    directory = paths['fast_commit_fixtures'] / profile
    test('fast-commit-decoder-' + profile, programs['fast-commit'],
      args: [directory / 'pending.journal', '7', '3'], timeout: 300, suite: 'core')
    test('fast-commit-recovery-' + profile, programs['fast-commit-recovery'],
      args: [directory / 'pending.img', directory / 'expected.img'], timeout: 300, suite: 'core')
    if profile in ['orphan-1k', 'orphan-4k']
      test('fast-commit-orphan-guards-' + profile, programs['fast-commit-recovery'],
        args: [directory / 'pending.img', directory / 'expected.img', '--orphans'],
        timeout: 300, suite: 'core')
    endif
    if profile in ['1k', '4k', 'orphan-1k', 'orphan-4k']
      test('fast-commit-resources-' + profile, programs['fast-commit-recovery'],
        args: [directory / 'pending.img', directory / 'expected.img', '--resources'],
        timeout: 600, suite: 'core')
      test('fast-commit-faults-' + profile, programs['fast-commit-recovery'],
        args: [directory / 'pending.img', directory / 'expected.img', '--faults'],
        timeout: 600, suite: 'core')
    endif
  endforeach
endif

if paths['external_journal_fixtures'] != ''
  foreach profile : ['legacy-1k', 'v1-4k', 'v2-1k', 'v3-4k', 'v3-64k']
    stem = paths['external_journal_fixtures'] / profile
    test('external-journal-' + profile, programs['external-journal'],
      args: [stem + '.img', stem + '.journal'], timeout: 300, suite: 'core')
    if profile in ['v2-1k', 'v3-4k']
      test('external-journal-faults-' + profile, programs['external-journal'],
        args: ['--faults', stem + '.img', stem + '.journal'], timeout: 600, suite: 'core')
    endif
  endforeach
endif

if paths['cluster_fixtures'] != ''
  test('cluster-private-values', programs['cluster'],
    args: ['--values', paths['cluster_fixtures'] / 'cluster-ea-inode.img'],
    timeout: 300, suite: 'core')
  foreach profile : ['1k', '4k', 'wide', 'no-checksum', 'checksum-seed', 'inline', 'ea-inode', 'orphan-file']
    test('cluster-mutation-' + profile, programs['cluster'],
      args: paths['cluster_fixtures'] / ('cluster-' + profile + '.img'), timeout: 600, suite: 'core')
    if profile in ['1k', '4k']
      test('cluster-faults-' + profile, programs['cluster'],
        args: ['--faults', paths['cluster_fixtures'] / ('cluster-' + profile + '.img')],
        timeout: 600, suite: 'core')
    endif
  endforeach
endif

if paths['large_volume_fixtures'] != ''
  foreach profile : ['1k-high', '1k-meta-high', '4k-boundary']
    test('large-volume-reader-' + profile, programs['large-volume'],
      args: ['--read', paths['large_volume_fixtures'] / ('volume-' + profile + '.img')],
      timeout: 300, suite: 'core')
  endforeach
endif

if paths['large_file_fixtures'] != ''
  foreach profile : ['1k', '4k', '64k', 'indirect', 'inline', 'cluster', 'cluster-inline', 'no-huge', 'indirect-no-huge']
    test('large-file-mutation-' + profile, programs['large-file'],
      args: paths['large_file_fixtures'] / ('large-file-' + profile + '.img'), timeout: 300, suite: 'core')
  endforeach
endif

if paths['cluster_space_fixtures'] != ''
  foreach profile : ['1k', '4k']
    test('cluster-full-space-' + profile, programs['cluster'],
      args: ['--full', paths['cluster_space_fixtures'] / ('cluster-' + profile + '.img')],
      timeout: 300, suite: 'core')
  endforeach
endif

if paths['inline_fixtures'] != ''
  foreach profile : ['1k', '4k', 'no-checksum', 'checksum-seed', 'no-filetype', 'inode512', 'ea-inode', 'orphan-file']
    test('inline-mutation-' + profile, programs['inline'],
      args: paths['inline_fixtures'] / ('inline-' + profile + '.img'), timeout: 300, suite: 'core')
    if profile in ['1k', '4k']
      test('inline-faults-' + profile, programs['inline'],
        args: ['--faults', paths['inline_fixtures'] / ('inline-' + profile + '.img')],
        timeout: 600, suite: 'core')
    endif
  endforeach
endif

if paths['inline_space_fixtures'] != ''
  foreach profile : ['1k', '4k']
    test('inline-space-' + profile, programs['inline'],
      args: ['--full', paths['inline_space_fixtures'] / ('inline-' + profile + '.img')],
      timeout: 300, suite: 'core')
  endforeach
endif

xattr_profiles = ['1k', '4k', '16k', '64k', 'indirect', 'no-checksum',
  'checksum-seed', 'inode128', 'inode512', 'orphan-file']
if paths['ea_inode_fixtures'] != ''
  foreach profile : ['1k', '4k', 'no-checksum', 'checksum-seed', 'inode128', 'orphan-file']
    stem = paths['ea_inode_fixtures'] / ('ea-inode-' + profile)
    test('ea-inode-reader-' + profile, programs['xattr'],
      args: ['--verify', stem + '.img', stem + '.xattrs'], timeout: 300, suite: 'core')
    test('ea-inode-mutation-' + profile, programs['ea-inode'],
      args: stem + '.img', timeout: 600, suite: 'core')
    test('ea-inode-orphans-' + profile, programs['ea-inode'],
      args: ['--value-orphans', stem + '.img'], timeout: 300, suite: 'core')
    if profile in ['1k', 'inode128']
      test('ea-inode-faults-' + profile, programs['ea-inode'],
        args: ['--faults', stem + '.img'], timeout: 600, suite: 'core')
    endif
  endforeach
endif
if paths['xattr_fixtures'] != ''
  foreach profile : xattr_profiles
    stem = paths['xattr_fixtures'] / ('xattr-' + profile)
    test('xattr-reader-' + profile, programs['xattr'],
      args: [stem + '.img', stem + '.expected'], timeout: 300, suite: 'core')
    test('xattr-mutation-' + profile, programs['xattr-write'],
      args: stem + '.img', timeout: 600, suite: 'core')
    test('xattr-packing-' + profile, programs['xattr-write'],
      args: ['--edges', stem + '.img'], timeout: 300, suite: 'core')
    test('xattr-lifetime-' + profile, programs['xattr-lifetime'],
      args: stem + '.img', timeout: 600, suite: 'core')
    test('inode-flags-' + profile, programs['inode-flags'],
      args: stem + '.img', timeout: 300, suite: 'core')
  endforeach
  foreach profile : ['1k', 'inode128']
    test('inode-flags-faults-' + profile, programs['inode-flags'],
      args: ['--faults', paths['xattr_fixtures'] / ('xattr-' + profile + '.img')],
      timeout: 300, suite: 'core')
  endforeach
endif
if paths['xattr_enable_fixtures'] != ''
  foreach profile : xattr_profiles
    test('xattr-first-attribute-' + profile, programs['xattr-lifetime'],
      args: ['--enable', paths['xattr_enable_fixtures'] / ('xattr-enable-' + profile + '.img')],
      timeout: 600, suite: 'core')
  endforeach
endif
if paths['xattr_space_fixtures'] != ''
  foreach profile : xattr_profiles
    test('xattr-full-space-' + profile, programs['xattr-write'],
      args: ['--full', paths['xattr_space_fixtures'] / ('xattr-space-' + profile + '.img')],
      timeout: 300, suite: 'core')
  endforeach
endif
if paths['space_fixtures'] != ''
  foreach profile : ['1k', '4k', 'indirect', 'no-checksum', 'no-filetype',
    'inode128', 'checksum-seed', 'orphan-file']
    test('namespace-space-' + profile, programs['space'],
      args: paths['space_fixtures'] / ('space-' + profile + '.img'),
      timeout: 300, suite: 'namespace')
    if profile != 'indirect'
      test('preallocation-full-space-' + profile, programs['file-range'],
        args: ['--capacity-full', paths['space_fixtures'] / ('space-' + profile + '.img')],
        timeout: 300, suite: 'namespace')
      test('preallocation-keep-size-' + profile, programs['file-range'],
        args: ['--capacity-keep', paths['space_fixtures'] / ('space-' + profile + '.img')],
        timeout: 300, suite: 'namespace')
      test('preallocation-reservation-' + profile, programs['file-range'],
        args: ['--reservation', paths['space_fixtures'] / ('space-' + profile + '.img')],
        timeout: 300, suite: 'namespace')
    endif
  endforeach
endif

if paths['space_fixtures'] != ''
  foreach profile : ['1k', '4k']
    test('preallocation-reservation-tree-' + profile, programs['file-range'],
      args: ['--reservation-tree', paths['space_fixtures'] / ('space-' + profile + '.img')],
      timeout: 300, suite: 'namespace')
  endforeach
  foreach profile : ['1k', 'inode128']
    test('preallocation-reservation-faults-' + profile, programs['file-range'],
      args: ['--reservation-faults', paths['space_fixtures'] / ('space-' + profile + '.img')],
      timeout: 600, suite: 'namespace')
  endforeach
  test('preallocation-reservation-controls', programs['file-range'],
    args: ['--reservation-controls', paths['space_fixtures'] / 'space-1k.img'],
    timeout: 300, suite: 'namespace')
  test('preallocation-full-space-large', programs['file-range'],
    args: ['--capacity', paths['space_fixtures'] / 'space-1k.img'], timeout: 300, suite: 'namespace')
  test('preallocation-full-space-faults', programs['file-range'],
    args: ['--capacity-faults', paths['space_fixtures'] / 'space-1k.img'], timeout: 600, suite: 'namespace')
  test('preallocation-full-space-tree', programs['file-range'],
    args: ['--capacity-tree', paths['space_fixtures'] / 'space-1k.img'], timeout: 300, suite: 'namespace')
  test('preallocation-keep-size-large', programs['file-range'],
    args: ['--capacity-keep-large', paths['space_fixtures'] / 'space-1k.img'], timeout: 300, suite: 'namespace')
  test('preallocation-keep-size-tree', programs['file-range'],
    args: ['--capacity-keep-tree', paths['space_fixtures'] / 'space-1k.img'], timeout: 300, suite: 'namespace')
  test('preallocation-keep-size-faults', programs['file-range'],
    args: ['--capacity-keep-faults', paths['space_fixtures'] / 'space-1k.img'], timeout: 600, suite: 'namespace')
  test('preallocation-keep-size-prefix', programs['file-range'],
    args: ['--capacity-keep-prefix', paths['space_fixtures'] / 'space-1k.img'], timeout: 300, suite: 'namespace')
endif

test('directory-hash', programs['directory-hash'], suite: 'indexed')
if paths['index_capacity'] != ''
  test('indexed-capacity', programs['index-write'], args: ['--capacity', paths['index_capacity']],
    timeout: 300, suite: 'indexed')
  test('directory-lookup-capacity', programs['directory-lookup'],
    args: [paths['index_capacity'], paths['index_capacity'].replace('.img', '.lookup')],
    timeout: 300, suite: 'indexed')
endif
if paths['index_fixtures'] != ''
  foreach profile : ['legacy-signed', 'legacy-unsigned', 'half_md4-signed', 'half_md4-unsigned',
    'tea-signed', 'tea-unsigned', '2k', '4k', '8k', '16k', '32k', 'deep', 'deep-unsigned',
    'indirect', 'no-checksum', 'no-filetype', 'inode128', 'checksum-seed',
    'orphan-file', 'orphan-file-deep']
    image = paths['index_fixtures'] / ('index-' + profile + '.img')
    test('directory-index-' + profile, programs['directory-index'], args: image,
      timeout: 300, suite: 'indexed')
    test('directory-lookup-' + profile, programs['directory-lookup'],
      args: [image, paths['index_fixtures'] / ('index-' + profile + '.lookup')],
      timeout: 300, suite: 'indexed')
    test('indexed-write-' + profile, programs['index-write'], args: image,
      timeout: 300, suite: 'indexed')
    test('indexed-faults-' + profile, programs['index-write'], args: ['--faults', image],
      timeout: 900, suite: 'indexed')
    if paths['index_collisions'] != '' and profile not in ['2k', '4k', '8k', '16k', '32k']
      test('indexed-edges-' + profile, programs['index-write'],
        args: ['--edges', paths['index_collisions'], image], timeout: 300, suite: 'indexed')
    endif
  endforeach
endif

if paths['large_directory_fixtures'] != ''
  foreach profile : ['index-large-dir', 'index-large-dir-indirect', 'index-large-dir-gdt',
    'index-large-dir-capacity']
    image = paths['large_directory_fixtures'] / (profile + '.img')
    test('large-dir-graph-' + profile, programs['directory-index'],
      args: ['--scan-only', image], timeout: 300, suite: 'indexed')
    test('large-dir-lookup-' + profile, programs['directory-lookup'],
      args: [image, paths['large_directory_fixtures'] / (profile + '.lookup')],
      timeout: 300, suite: 'indexed')
    if profile != 'index-large-dir-capacity'
      test('large-dir-write-' + profile, programs['index-write'],
        args: image, timeout: 600, suite: 'indexed')
    else
      test('large-dir-full-root-growth', programs['index-write'],
        args: ['--fault-smoke', '--large-split', 'grow', image], timeout: 300, suite: 'indexed')
    endif
  endforeach
endif
if paths['large_directory_edges'] != ''
  capacities = []
  foreach profile : ['index-large-dir', 'index-large-dir-indirect', 'index-large-dir-gdt']
    stem = paths['large_directory_edges'] / profile
    test('large-dir-structure-' + profile, programs['directory-index'],
      args: stem + '-cascade.img', timeout: 300, suite: 'indexed')
    test('large-dir-lookup-cascade-' + profile, programs['directory-lookup'],
      args: [stem + '-cascade.img', stem + '-cascade.lookup'], timeout: 300, suite: 'indexed')
    foreach transition : ['grow', 'cascade']
      test('large-dir-faults-' + transition + '-' + profile, programs['index-write'],
        args: ['--faults', '--large-split', transition, stem + '-' + transition + '.img'],
        timeout: 900, suite: 'indexed')
    endforeach
    capacities += stem + '-capacity.img'
  endforeach
  test('large-dir-capacity', programs['index-write'], args: ['--capacity'] + capacities,
    timeout: 300, suite: 'indexed')
endif

reader_names = ['ext4-4k.img', 'ext4-1k.img', 'ext4-indexed.img']
write_names = ['ext4-4k.img', 'ext4-1k.img']
if get_option('extended_tests')
  extended = ['ext4-2k.img', 'ext4-8k.img', 'ext4-16k.img', 'ext4-32k.img',
    'ext4-indirect-1k.img', 'ext4-no-checksum.img', 'ext4-checksum-seed.img']
  reader_names += ['ext4-2k.img', 'ext4-8k.img', 'ext4-16k.img', 'ext4-32k.img',
    'ext4-64k.img', 'ext4-indirect-1k.img', 'ext4-no-checksum.img', 'ext4-checksum-seed.img']
  write_names += extended
endif
if get_option('inode128_test')
  write_names += 'ext4-inode128.img'
endif
reader_images = []
foreach name : reader_names
  reader_images += paths['fixtures'] / name
endforeach
write_images = []
foreach name : write_names
  write_images += paths['fixtures'] / name
endforeach
foreach name : write_names
  special_smoke = name in ['ext4-1k.img', 'ext4-4k.img'] ? [] : ['--smoke']
  test('special-files-' + name, programs['namespace'],
    args: ['--special'] + special_smoke + [paths['fixtures'] / name],
    timeout: 900, suite: 'namespace')
  test('whiteout-rename-' + name, programs['rename'],
    args: ['--whiteout'] + special_smoke + [paths['fixtures'] / name],
    timeout: 900, suite: 'rename')
endforeach
test('special-files-indexed', programs['namespace'],
  args: ['--special', '--indexed', '--smoke', paths['fixtures'] / 'ext4-indexed.img'],
  timeout: 300, suite: 'namespace')
if get_option('namespace_tests')
  test('special-files-no-filetype', programs['namespace'],
    args: ['--special', '--smoke', paths['namespace_fixtures'] / 'namespace-no-filetype.img'],
    timeout: 300, suite: 'namespace')
  test('whiteout-rename-no-filetype', programs['rename'],
    args: ['--whiteout', '--smoke', paths['namespace_fixtures'] / 'namespace-no-filetype.img'],
    timeout: 300, suite: 'rename')
endif
if paths['group_checksum_fixtures'] != ''
  test('special-files-group-checksum', programs['namespace'],
    args: ['--special', '--smoke', paths['group_checksum_fixtures'] / 'gdt-1k32.img'],
    timeout: 300, suite: 'core')
  test('whiteout-rename-group-checksum', programs['rename'],
    args: ['--whiteout', '--smoke', paths['group_checksum_fixtures'] / 'gdt-1k32.img'],
    timeout: 300, suite: 'core')
endif
if paths['directory_index_64k_fixture'] != ''
  test('directory-create-index-64k', programs['index-write'],
    args: ['--create-index', '--fault-smoke', paths['directory_index_64k_fixture']],
    timeout: 300, suite: 'indexed')
endif
if get_option('namespace_tests')
  test('directory-create-index-no-filetype', programs['index-write'],
    args: ['--create-index', '--fault-smoke', paths['namespace_fixtures'] / 'namespace-no-filetype.img'],
    timeout: 300, suite: 'indexed')
endif
foreach name : write_names
  test('directory-create-index-' + name, programs['index-write'],
    args: ['--create-index', '--fault-smoke', paths['fixtures'] / name],
    timeout: 300, suite: 'indexed')
endforeach
foreach profile : ['1k', '4k']
  test('directory-create-index-faults-' + profile, programs['index-write'],
    args: ['--create-index', paths['fixtures'] / ('ext4-' + profile + '.img')],
    timeout: 600, suite: 'indexed')
  test('directory-links-faults-' + profile, programs['index-write'],
    args: ['--directory-links', paths['fixtures'] / ('ext4-' + profile + '.img')],
    timeout: 600, suite: 'indexed')
endforeach
test('directory-create-index-hashes', programs['index-write'],
  args: ['--creation-hashes', paths['fixtures'] / 'ext4-1k.img'], timeout: 300, suite: 'indexed')
if paths['directory_links_fixture'] != ''
  test('directory-links-real-boundary', programs['index-write'],
    args: ['--directory-links-image', paths['directory_links_fixture']],
    timeout: 600, suite: 'indexed')
endif
test('metadata-checksum', programs['checksum'], timeout: 60, suite: 'core')
if paths['geometry_fixtures'] != ''
  foreach profile : ['meta-1k', 'meta-32', 'meta-4k', 'meta-gdt', 'meta-hybrid',
    'meta-sparse2', 'sparse2-0', 'sparse2-1', 'sparse2-2']
    stem = paths['geometry_fixtures'] / profile
    test('group-geometry-' + profile, programs['geometry'],
      args: [stem + '.img', stem + '.geometry'], timeout: 300, suite: 'core')
  endforeach
  foreach profile : ['meta-hybrid', 'meta-sparse2']
    stem = paths['geometry_fixtures'] / profile
    test('group-geometry-faults-' + profile, programs['geometry'],
      args: ['--faults', stem + '.img', stem + '.geometry'], timeout: 900, suite: 'core')
  endforeach
endif
if paths['group_checksum_fixtures'] != ''
  gdt = paths['group_checksum_fixtures']
  gdt_readers = [gdt / 'gdt-1k32.img', gdt / 'gdt-4k64.img', gdt / 'gdt-indirect.img']
  gdt_writers = gdt_readers + [gdt / 'gdt-inode128.img']
  gdt_small = [gdt / 'gdt-small64.img', gdt / 'gdt-small-indirect.img']
  # One format package owns these checks, including lazy allocation/recovery.
  foreach item : [
    ['reader', 'reader', gdt_readers],
    ['write', 'write', gdt_writers],
    ['allocation', 'write', ['--allocation'] + gdt_writers],
    ['truncate', 'write', ['--truncate'] + gdt_writers],
    ['partial-write', 'write-partial', gdt_writers],
    ['growth', 'write-growth', gdt_writers],
    ['growth-faults', 'write-growth', ['--faults', gdt / 'gdt-1k32.img']],
    ['orphan-recovery', 'orphan', [gdt / 'gdt-1k32.img', gdt / 'gdt-indirect.img']],
    ['live-truncate', 'orphan', ['--live', gdt / 'gdt-1k32.img', gdt / 'gdt-indirect.img']],
    ['namespace', 'namespace', gdt_small],
    ['inode-exhaustion', 'namespace', ['--exhaust'] + gdt_small],
    ['inode-groups', 'namespace', ['--groups'] + gdt_small],
    ['removal', 'removal', gdt_small],
    ['rename', 'rename', gdt_small],
  ]
    test('group-checksum-' + item[0], programs[item[1]], args: item[2],
      timeout: 900, suite: 'core')
  endforeach
endif
test('image-reader', programs['reader'], args: reader_images, timeout: 300, suite: 'core')
test('file-read-ranges', programs['read-ranges'], timeout: 60, suite: 'core')
test('malformed-images', programs['corruption'], args: paths['fixtures'] / 'ext4-4k.img',
  timeout: 300, suite: 'core')
foreach profile : ['4k', '1k']
  test('journal-durability-' + profile, programs['journal'],
    args: paths['fixtures'] / ('ext4-' + profile + '.img'), timeout: 300, suite: 'core')
  test('journal-checksum-v1-' + profile, programs['journal'],
    args: ['--checksum-v1', paths['fixtures'] / ('ext4-' + profile + '.img')], timeout: 300, suite: 'core')
  test('journal-async-' + profile, programs['journal'],
    args: ['--async', paths['fixtures'] / ('ext4-' + profile + '.img')], timeout: 300, suite: 'core')
  test('journal-async-v1-' + profile, programs['journal'],
    args: ['--checksum-v1', '--async', paths['fixtures'] / ('ext4-' + profile + '.img')], timeout: 300, suite: 'core')
endforeach
test('inode-and-file-write', programs['write'], args: write_images, timeout: 300, suite: 'core')
test('file-write-partial', programs['write-partial'], args: write_images, timeout: 300, suite: 'core')
foreach name : write_names
  test('file-range-' + name, programs['file-range'], args: paths['fixtures'] / name,
    timeout: 300, suite: 'core')
endforeach
foreach profile : ['ext4-1k.img', 'ext4-indirect-1k.img']
  if profile in write_names
    test('file-range-faults-' + profile, programs['file-range'],
      args: ['--faults', paths['fixtures'] / profile], timeout: 600, suite: 'core')
  endif
endforeach
test('file-growth-preallocation', programs['write-growth'], args: write_images,
  timeout: 300, suite: 'core')
foreach profile : ['4k', '1k']
  test('file-write-partial-faults-' + profile, programs['write-partial'],
    args: ['--faults', paths['fixtures'] / ('ext4-' + profile + '.img')], timeout: 600, suite: 'core')
endforeach
growth_fault_profiles = ['4k']
if get_option('extended_tests')
  growth_fault_profiles += ['indirect-1k']
endif
foreach profile : growth_fault_profiles
  test('file-growth-faults-' + profile, programs['write-growth'],
    args: ['--faults', paths['fixtures'] / ('ext4-' + profile + '.img')], timeout: 600, suite: 'core')
endforeach
test('allocation-and-growth', programs['write'], args: ['--allocation'] + write_images,
  timeout: 600, suite: 'core')
test('truncate-and-free', programs['write'], args: ['--truncate'] + write_images,
  timeout: 600, suite: 'core')
test('namespace-create-and-link', programs['namespace'], args: write_images,
  timeout: 900, suite: 'namespace')
test('namespace-symlinks', programs['namespace'], args: ['--symlinks'] + write_images,
  timeout: 900, suite: 'namespace')
foreach item : [
  ['namespace-removal-indexed-validation', 'removal', 'removal'],
  ['namespace-rename-indexed', 'rename', 'rename'],
  ['namespace-indexed', 'namespace', 'namespace'],
]
  test(item[0], programs[item[1]], args: ['--indexed', paths['fixtures'] / 'ext4-indexed.img'],
    timeout: 300, suite: item[2])
endforeach

removal_images = write_images
if get_option('namespace_tests')
  namespace_images = []
  foreach profile : ['small', 'lazy', 'indirect', 'no-checksum', 'no-filetype', 'inode128']
    namespace_images += paths['namespace_fixtures'] / ('namespace-' + profile + '.img')
  endforeach
  foreach name, arguments : {
    'namespace-small-formats': [],
    'namespace-inode-exhaustion': ['--exhaust'],
    'namespace-inode-group-transition': ['--groups'],
    'namespace-symlink-small-formats': ['--symlinks'],
    'namespace-symlink-inode-groups': ['--symlinks', '--groups'],
  }
    test(name, programs['namespace'], args: arguments + namespace_images,
      timeout: 600, suite: 'namespace')
  endforeach
  removal_images += namespace_images
endif
test('orphan-truncate-recovery', programs['orphan'], args: write_images,
  timeout: 600, suite: 'core')
test('live-truncate', programs['orphan'], args: ['--live'] + write_images,
  timeout: 900, suite: 'core')
if get_option('orphan_file_tests')
  orphan_images = []
  foreach name : write_names
    orphan_images += paths['orphan_file_fixtures'] / ('orphan-file-' + name)
  endforeach
  test('orphan-file-recovery', programs['orphan'], args: ['--orphan-file'] + orphan_images,
    timeout: 900, suite: 'orphan-file')
  test('orphan-file-mixed-recovery', programs['orphan'], args: ['--mixed',
    paths['orphan_file_fixtures'] / 'orphan-file-ext4-1k.img',
    paths['orphan_file_fixtures'] / 'orphan-file-ext4-4k.img'], timeout: 900, suite: 'orphan-file')
  test('orphan-file-live-truncate', programs['orphan'], args: ['--live'] + orphan_images,
    timeout: 900, suite: 'orphan-file')
  test('namespace-orphan-file', programs['namespace'], args: orphan_images,
    timeout: 900, suite: 'namespace')
  test('namespace-symlink-orphan-file', programs['namespace'], args: ['--symlinks'] + orphan_images,
    timeout: 900, suite: 'namespace')
  removal_images += orphan_images
  if get_option('extended_tests')
    test('orphan-file-indirect-recovery', programs['orphan'], args: ['--orphan-file',
      paths['orphan_file_fixtures'] / 'mapped/orphan-file-ext4-indirect-1k.img'],
      timeout: 900, suite: 'orphan-file')
    test('orphan-file-size-bound', programs['orphan'], args: ['--orphan-file', '--smoke',
      paths['orphan_file_fixtures'] / 'maximum/orphan-file-ext4-indirect-1k.img'],
      timeout: 900, suite: 'orphan-file')
  endif
endif
# Each image retains its full functional/fault matrix and its own deadline.
foreach image : removal_images
  profile = image.split('/')[-1].replace('.img', '')
  test('namespace-removal-' + profile, programs['removal'], args: image,
    timeout: 900, suite: 'removal')
  test('namespace-rename-' + profile, programs['rename'], args: image,
    timeout: 900, suite: 'rename')
endforeach
if get_option('extended_tests')
  test('live-truncate-large', programs['orphan'], args: ['--live', '--large',
    paths['fixtures'] / 'ext4-indirect-1k.img'], timeout: 900, suite: 'core')
endif
if get_option('unwritten_tests')
  unwritten_images = []
  foreach profile : ['1k', '4k', 'no-checksum', 'checksum-seed']
    unwritten_images += paths['allocation_fixtures'] / ('unwritten-ext4-' + profile + '.img')
  endforeach
  test('live-truncate-unwritten', programs['orphan'], args: ['--live'] + unwritten_images,
    timeout: 600, suite: 'core')
  test('unwritten-allocation', programs['write'], args: ['--allocation'] + unwritten_images,
    timeout: 300, suite: 'core')
  test('unwritten-truncate', programs['write'], args: ['--truncate'] + unwritten_images,
    timeout: 300, suite: 'core')
endif
