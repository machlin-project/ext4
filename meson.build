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
  'core/support.c', 'core/mount.c', 'core/inode.c', 'core/read.c', 'core/directory.c',
  'core/directory_lookup.c',
  'core/journal.c', 'core/recovery.c', 'core/group.c', 'core/system.c',
  'core/allocate.c', 'core/map_write.c', 'core/write.c', 'core/orphan.c',
  'core/inode_allocate.c', 'core/namespace.c', 'core/lifetime.c',
  'core/directory_hash.c', 'core/directory_index.c', 'core/directory_write.c',
  'core/xattr.c', 'core/xattr_write.c',
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
  'xattr': 'xattr', 'xattr-write': 'xattr_write', 'xattr-lifetime': 'xattr_lifetime', 'space': 'space',
  'directory-index': 'directory_index', 'directory-lookup': 'directory_lookup', 'index-write': 'index_write',
  'reader': 'reader', 'read-ranges': 'read_ranges', 'corruption': 'corruption', 'journal': 'journal',
  'write': 'write', 'namespace': 'namespace', 'removal': 'removal',
  'rename': 'rename', 'orphan': 'orphan',
}
  programs += {name: executable('ext4-' + name + '-test', 'tests/' + source + '.c',
    include_directories: internal_headers, dependencies: posix_dependency)}
endforeach
programs += {'directory-hash': executable('ext4-directory-hash-test', 'tests/directory_hash.c',
  include_directories: internal_headers, dependencies: ext4_dependency)}
executable('ext4-recover', 'tools/recover.c', dependencies: posix_dependency)
executable('ext4-read-benchmark', ['tools/benchmark_read.c', 'adapters/posix/image.c'],
  include_directories: [public_headers, posix_headers], c_args: posix_arguments,
  link_with: freestanding_core,
  override_options: ['optimization=2', 'b_sanitize=none', 'b_coverage=false', 'b_lto=false'])

threads = dependency('threads')
foreach name : ['mounted', 'mounted-symlink']
  source = name == 'mounted' ? 'mounted.c' : 'mounted_symlinks.c'
  executable('ext4-' + name + '-test', 'tests/' + source, dependencies: threads,
    override_options: ['b_sanitize=none'])
endforeach
if host_machine.system() == 'darwin'
  executable('ext4-mounted-lifetime-test', 'tests/mounted_lifetime.c',
    override_options: ['b_sanitize=none'])
endif

if get_option('fuzzer')
  if cc.get_id() not in ['clang', 'apple-clang']
    error('The image fuzzer requires Clang with a libFuzzer runtime.')
  endif
  executable('ext4-image-fuzzer', 'tests/fuzz_image.c', core_sources,
    include_directories: [internal_headers, public_headers],
    c_args: ['-fsanitize=fuzzer'], link_args: ['-fsanitize=fuzzer'],
    override_options: ['b_sanitize=address,undefined'])
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
