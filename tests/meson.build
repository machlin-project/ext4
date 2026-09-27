# SPDX-License-Identifier: BSD-3-Clause
# Every test belongs to exactly one CI suite. Selecting a fixture profile adds
# required cases; missing images fail tests instead of silently removing them.
paths = {}
foreach option : ['fixtures', 'allocation_fixtures', 'orphan_file_fixtures',
  'namespace_fixtures', 'xattr_fixtures', 'xattr_space_fixtures', 'xattr_enable_fixtures', 'space_fixtures',
  'index_fixtures', 'index_collisions', 'index_capacity', 'group_checksum_fixtures',
  'large_directory_fixtures', 'large_directory_edges']
  value = get_option(option)
  paths += {option: value == '' ? '' : join_paths(meson.project_source_root(), value)}
endforeach

xattr_profiles = ['1k', '4k', '16k', '64k', 'indirect', 'no-checksum',
  'checksum-seed', 'inode128', 'inode512', 'orphan-file']
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
  endforeach
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
test('metadata-checksum', programs['checksum'], timeout: 60, suite: 'core')
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
endforeach
test('inode-and-file-write', programs['write'], args: write_images, timeout: 300, suite: 'core')
test('file-write-partial', programs['write-partial'], args: write_images, timeout: 300, suite: 'core')
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
