# SPDX-License-Identifier: BSD-3-Clause
project('machlin_ext4', 'c', version: '0.1.0', meson_version: '>=1.3.0',
  default_options: ['c_std=c11', 'buildtype=debugoptimized', 'warning_level=2',
    'werror=true', 'b_sanitize=address,undefined', 'b_lundef=false'])

cc = meson.get_compiler('c')
add_project_arguments('-Wdeclaration-after-statement', language: 'c')
if cc.get_id() in ['clang', 'apple-clang']
  add_project_arguments('-Werror=conditional-uninitialized', language: 'c')
endif

core_sources = files(
  'core/support.c', 'core/memory.c', 'core/mount.c', 'core/inode.c',
  'core/read.c', 'core/read_io.c', 'core/read_state.c',
  'core/map_read.c', 'core/seek.c', 'core/directory.c',
  'core/directory_lookup.c', 'core/inline.c', 'core/inline_create.c',
  'core/journal.c', 'core/transaction.c', 'core/journal_read.c',
  'core/recovery.c', 'core/group.c', 'core/system.c',
  'core/fast_commit.c', 'core/fast_commit_replay.c',
  'core/allocate.c', 'core/cluster.c', 'core/map_write.c', 'core/write.c', 'core/orphan.c',
  'core/inode_allocate.c', 'core/inode_flags.c', 'core/namespace.c', 'core/lifetime.c',
  'core/directory_hash.c', 'core/directory_index.c', 'core/directory_write.c',
  'core/xattr.c', 'core/xattr_write.c', 'core/xattr_inode.c', 'core/mmp.c',
  'core/sha.c', 'core/verity.c', 'core/verity_enable.c', 'core/fscrypt.c', 'core/unicode.c', 'core/quota.c',
)
core_arguments = ['-Wpedantic', '-ffreestanding', '-fno-builtin']
public_headers = include_directories('include')
internal_headers = include_directories('core')
posix_headers = include_directories('adapters/posix')

ext4_core = static_library('ext4_core', core_sources,
  include_directories: public_headers, c_args: core_arguments)
ext4_dependency = declare_dependency(link_with: ext4_core,
  include_directories: public_headers)

# Instrumentation changes frame layout. Compile the same sources separately at
# O2 without sanitizers, coverage or LTO so every build checks the kernel budget.
freestanding_core = static_library('ext4_freestanding_check', core_sources,
  include_directories: public_headers,
  c_args: core_arguments + ['-Wframe-larger-than=2048'], pic: false,
  override_options: ['optimization=2', 'b_sanitize=none', 'b_coverage=false', 'b_lto=false'])

posix_arguments = ['-D_POSIX_C_SOURCE=200809L']
if host_machine.system() == 'darwin'
  posix_arguments += '-D_DARWIN_C_SOURCE'
endif
ext4_posix = static_library('ext4_posix', 'adapters/posix/image.c',
  include_directories: posix_headers, dependencies: ext4_dependency,
  c_args: posix_arguments)
posix_dependency = declare_dependency(link_with: ext4_posix,
  include_directories: posix_headers, dependencies: ext4_dependency)

# Keep executable paths stable for the independent image and VM harnesses.
programs = {}
foreach name, source : {
  'checksum': 'checksum', 'geometry': 'geometry', 'ranges': 'ranges', 'hash': 'hash', 'memory': 'memory',
  'xattr': 'xattr', 'xattr-write': 'xattr_write', 'xattr-lifetime': 'xattr_lifetime', 'space': 'space',
  'ea-inode': 'ea_inode', 'inline': 'inline', 'cluster': 'cluster',
  'directory-index': 'directory_index', 'directory-lookup': 'directory_lookup', 'index-write': 'index_write',
  'reader': 'reader', 'read-ranges': 'read_ranges', 'read-held': 'read_held',
  'corruption': 'corruption', 'journal': 'journal', 'journal-read': 'journal_read',
  'journal-transfer': 'journal_transfer',
  'external-journal': 'external_journal', 'unjournaled': 'unjournaled',
  'fast-commit': 'fast_commit', 'fast-commit-recovery': 'fast_commit_recovery',
  'write': 'write', 'write-partial': 'write_partial', 'write-request': 'write_request', 'write-growth': 'write_growth',
  'file-range': 'file_range', 'large-file': 'large_file', 'large-volume': 'large_volume', 'inode-flags': 'inode_flags',
  'namespace': 'namespace', 'removal': 'removal', 'sustained': 'sustained', 'mmp': 'mmp',
  'deferred': 'deferred',
  'verity': 'verity', 'verity-enable': 'verity_enable', 'encrypt': 'encrypt', 'casefold': 'casefold', 'casefold-fs': 'casefold_fs', 'quota': 'quota',
  'rename': 'rename', 'orphan': 'orphan',
}
  programs += {name: executable('ext4-' + name + '-test', 'tests/' + source + '.c',
    include_directories: internal_headers, dependencies: posix_dependency)}
endforeach
# Test the scalar SHA fallback even when the host enables hardware SHA rounds.
programs += {'hash-portable': executable('ext4-hash-portable-test',
  ['tests/hash.c', 'core/sha.c'], include_directories: internal_headers,
  dependencies: ext4_dependency, c_args: core_arguments + ['-DEXT4_SHA_PORTABLE'])}
programs += {'memory-portable': executable('ext4-memory-portable-test',
  ['tests/memory.c', 'core/memory.c'], include_directories: internal_headers,
  dependencies: ext4_dependency, c_args: core_arguments + ['-DEXT4_MEMORY_PORTABLE'])}
programs += {'directory-hash': executable('ext4-directory-hash-test', 'tests/directory_hash.c',
  include_directories: internal_headers, dependencies: ext4_dependency)}
programs += {'crypto-vectors': executable('ext4-crypto-vectors-test', 'tests/crypto_vectors.c',
  include_directories: internal_headers, dependencies: ext4_dependency)}
executable('ext4-recover', 'tools/recover.c', dependencies: posix_dependency)
executable('ext4-read-benchmark', ['tools/benchmark_read.c', 'adapters/posix/image.c'],
  include_directories: [public_headers, posix_headers], c_args: posix_arguments,
  link_with: freestanding_core,
  override_options: ['optimization=2', 'b_sanitize=none', 'b_coverage=false', 'b_lto=false'])
# Scale measurements run the optimized freestanding core on an in-memory image.
executable('ext4-scale', 'tools/scale.c', include_directories: public_headers,
  c_args: posix_arguments, link_with: freestanding_core,
  override_options: ['optimization=2', 'b_sanitize=none', 'b_coverage=false', 'b_lto=false'])
executable('ext4-hash-benchmark', 'tools/benchmark_hash.c',
  include_directories: [public_headers, internal_headers], link_with: freestanding_core,
  override_options: ['optimization=2', 'b_sanitize=none', 'b_coverage=false', 'b_lto=false'])

threads = dependency('threads')
foreach name : ['mounted', 'mounted-symlink']
  source = name == 'mounted' ? 'mounted.c' : 'mounted_symlinks.c'
  executable('ext4-' + name + '-test', 'tests/' + source, dependencies: threads,
    override_options: ['b_sanitize=none'])
endforeach
executable('ext4-mounted-manifest-test', 'tests/mounted_manifest.c',
  override_options: ['b_sanitize=none'])
if host_machine.system() == 'darwin'
  executable('ext4-mounted-lifetime-test', 'tests/mounted_lifetime.c',
    override_options: ['b_sanitize=none'])
  executable('ext4-mounted-failure-test', 'tests/mounted_failure.c',
    override_options: ['b_sanitize=none'])
  executable('ext4-mounted-removal-test', 'tests/mounted_removal.c',
    override_options: ['b_sanitize=none'])
  executable('ext4-mounted-capacity-test', 'tests/mounted_capacity.c',
    override_options: ['b_sanitize=none'])
endif

if get_option('fuzzer')
  if cc.get_id() not in ['clang', 'apple-clang']
    error('The image fuzzer requires Clang with a libFuzzer runtime.')
  endif
  foreach name, source : {'image': 'fuzz_image', 'operations': 'fuzz_operations'}
    executable('ext4-' + name + '-fuzzer', 'tests/' + source + '.c', core_sources,
      include_directories: [internal_headers, public_headers],
      c_args: ['-fsanitize=fuzzer'], link_args: ['-fsanitize=fuzzer'],
      override_options: ['b_sanitize=address,undefined'])
  endforeach
endif

subdir('tests')

python = find_program('python3')
run_target('format', command: [python, files('scripts/format.py')])
run_target('check-style', command: [python, files('scripts/format.py'), '--check'])
if host_machine.system() == 'darwin'
  # Native bundles retain their SDK-specific Xcode and kernel build tools.
  run_target('fskit', command: [python, files('scripts/build_fskit.py')])
  run_target('kext', command: [python, files('scripts/build_kext.py')])
endif
