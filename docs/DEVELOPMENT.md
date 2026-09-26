# Development

Use the selected Xcode C compiler and formatter on macOS. The portable core and
image tests must also compile with Clang on Linux. FSKit builds target a declared
macOS baseline; do not use newer SDK APIs without availability handling.

`make format` applies `.clang-format` to owned C headers, C sources and Objective-C
adapters. `make check-style` runs the same formatter without modifying files and
fails on formatting differences. Both select `clang-format` with `xcrun --find`
so `DEVELOPER_DIR` and the selected Xcode determine the toolchain. `CLANG_FORMAT`
is an explicit override; Linux checks require a compatible version 21 formatter.
The profile matches LXNU: tabs, eight-column indentation, a 100-column limit,
braced control flow and one statement per line. C declarations still belong at
block starts; formatting alone does not enforce declaration order.

External e2fsprogs commands create and independently inspect ext4 images. In the
shared workspace these tools are prepared under the lab's ignored `vendor/`
directory. Standalone users can point the fixture generator at their installed
tools. Images, dependency downloads and generated reports are ignored.

Generate the initial image profile with e2fsprogs tools on PATH:

```sh
python3 tests/generate_fixtures.py
make test
make check-style
```

For an out-of-tree e2fsprogs build, use
`--tools-root /absolute/path/to/e2fsprogs/build`. To retain previous evidence,
choose a new `--output` directory; the generator refuses to overwrite images.
`cmake -S . -B .build -DEXT4_FIXTURES=/absolute/path/to/fixtures` selects that
directory for tests. ASan/UBSan are enabled by default and can be disabled for
an adapter build with `-DEXT4_SANITIZERS=OFF`.

The selected Xcode clang compiles a second, optimized freestanding object target
with the same source and a 2048-byte frame-size check. This is a portability
check, not a linked or boot-tested kernel artifact. Kernel stack-protector symbols
remain enabled; XNU supplies them. Core code has no libc I/O or allocation imports.

Development proceeds through a portable reader and image tests, a stock FSKit
read-only mount, an early read-only kernel adapter, then transactional writes,
recovery and complete platform integration. Keep cheap userspace tests in the
iteration loop and periodically compile the same core for the kernel. Kernel
acceptance runs only in a coordinated disposable VM through Machlin lab.

The two platform builds share the same implementation. FSKit acceptance does
not establish kernel-stack safety, vnode/UBC ownership or LXNU correctness.

Use prepared bounded commands for execution workers. Preserve user changes in
the existing lab and XNU trees. The ongoing VFS refactor is an independent scope;
new filesystem work must not absorb or publish its uncommitted changes.
