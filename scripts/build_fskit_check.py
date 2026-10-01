#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Build the separate e2fsck resource helper without linking it into the core."""

import argparse
import json
import os
from pathlib import Path
import re
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[1]
UPSTREAM = "https://kernel.googlesource.com/pub/scm/fs/ext2/e2fsprogs"
UPSTREAM_TAG = "v1.47.4"
UPSTREAM_COMMIT = "7ee1d505ef3b37831215f490411f346fe57e9053"
MINIMUM_MACOS = "26.5"


def run(command, directory, environment=None):
    print(" ".join(map(str, command)), flush=True)
    subprocess.run(command, cwd=directory, env=environment, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT / "vendor/e2fsprogs-maintenance")
    parser.add_argument("--source-release", action="store_true",
                        help="Build supplied corresponding source without its Git database")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    args = parser.parse_args()
    source, output = args.source.resolve(), args.output.resolve()
    if not source.exists():
        if args.source_release:
            parser.error("--source-release requires an existing source directory")
        source.parent.mkdir(parents=True, exist_ok=True)
        run(["git", "clone", "--depth", "1", "--branch", UPSTREAM_TAG,
             UPSTREAM, str(source)], source.parent)
    if args.source_release:
        if not (source / 'configure').is_file() or not (source / 'e2fsck/unix.c').is_file():
            parser.error("The supplied release source is incomplete")
        revision = None
        print("Building explicitly supplied release source; Git pinning is disabled", flush=True)
    else:
        revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=source, text=True).strip()
        if revision != UPSTREAM_COMMIT:
            parser.error("The maintenance source must be the pinned upstream release")
        if subprocess.check_output(["git", "status", "--porcelain", "--untracked-files=no"], cwd=source):
            parser.error("The upstream source has tracked local changes")
    clang = subprocess.check_output(["xcrun", "--find", "clang"], text=True).strip()
    sdk = subprocess.check_output(["xcrun", "--sdk", "macosx", "--show-sdk-path"], text=True).strip()
    build = output / args.arch
    build.mkdir(parents=True, exist_ok=True)
    flags = ["-arch", args.arch, "-isysroot", sdk, "-mmacosx-version-min=" + MINIMUM_MACOS]
    environment = os.environ.copy()
    for key in ("CC", "CXX", "CFLAGS", "CPPFLAGS", "LDFLAGS", "SDKROOT", "PKG_CONFIG_PATH"):
        environment.pop(key, None)
    # Upstream's native type probe invokes CC without CFLAGS. Keep the selected
    # SDK in the compiler command so the probe also finds system headers.
    compiler = shlex.join([clang, '-isysroot', sdk])
    environment.update(CC=compiler, CFLAGS=shlex.join(flags + ["-O2"]),
                       LDFLAGS=shlex.join(flags), PKG_CONFIG_PATH="")
    # The generated build directory belongs to this one architecture and SDK.
    configuration = {"architecture": args.arch, "sdk": sdk, "compiler": compiler,
                     "upstream_commit": revision, "minimum_macos": MINIMUM_MACOS}
    configuration_file = build / "configuration.json"
    if configuration_file.exists() and json.loads(configuration_file.read_text()) != configuration:
        parser.error("Use a new output directory for a changed toolchain or dependency")
    if not (build / "Makefile").exists():
        host = "aarch64" if args.arch == "arm64" else "x86_64"
        run([str(source / "configure"), "--host=" + host + "-apple-darwin",
             "--disable-nls", "--disable-fuse2fs", "--disable-uuidd",
             "--without-libarchive", "--disable-imager", "--disable-resizer",
             "--disable-defrag", "--disable-e2initrd-helper", "--disable-backtrace"],
            build, environment)
        configuration_file.write_text(json.dumps(configuration, indent=2) + "\n")
    run(["make", "-j4", "libs"], build, environment)
    makefile = (build / "e2fsck/Makefile").read_text().replace("\\\n", " ")
    match = re.search(r"^OBJS\s*=\s*(.+)$", makefile, re.MULTILINE)
    if match is None:
        raise RuntimeError("The upstream e2fsck object list is missing")
    objects = match[1].split()
    if "$(MTRACE_OBJ)" in objects:
        objects.remove("$(MTRACE_OBJ)")
    if not objects or any(not re.fullmatch(r"[a-z0-9_]+\.o", item) for item in objects):
        raise RuntimeError("Unsupported upstream e2fsck object list")
    # Only the independent tool redirects its I/O manager and entry point.
    # Upstream source remains unchanged, including its library implementation.
    defines = ("-Dmain=ext4_e2fsck_main -Dunix_io_manager=ext4_maintenance_io_manager "
               "-Dext2fs_get_device_size2=ext4_maintenance_device_size")
    object_flags = environment["CFLAGS"] + " " + defines
    object_configuration = build / "e2fsck/object-flags.txt"
    if not object_configuration.exists() or object_configuration.read_text() != object_flags:
        for name in objects:
            (build / "e2fsck" / name).unlink(missing_ok=True)
    run(["make", "-j4", "CFLAGS=" + object_flags, *objects],
        build / "e2fsck", environment)
    object_configuration.write_text(object_flags)
    includes = ["-I" + str(build / "lib"), "-I" + str(source / "lib"), "-I" + str(build)]
    for owned in (ROOT / "tools/maintenance/main.c", ROOT / "tools/maintenance/io.c",
                  ROOT / "adapters/fskit/Ext4CheckWire.c"):
        run([clang, *flags, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
             "-Wdeclaration-after-statement", *includes, "-c", str(owned),
             "-o", str(build / (owned.stem + ".o"))], ROOT, environment)
    libraries = [build / "lib" / name for name in
                 ("libsupport.a", "libext2fs.a", "libcom_err.a", "libblkid.a", "libuuid.a", "libe2p.a")]
    archive = build / "libe2fsck-resource.a"
    run(["xcrun", "libtool", "-static", "-o", str(archive),
         *[str(build / "e2fsck" / name) for name in objects], *map(str, libraries)], ROOT, environment)
    run([clang, *flags, "-o", str(build / "ext4-check-resource"),
         str(build / "main.o"), str(build / "io.o"), str(build / "Ext4CheckWire.o"),
         str(archive), "-lpthread"], ROOT, environment)
    # The distributed source package includes upstream's complete license text.
    (build / "e2fsprogs-NOTICE").write_bytes((source / "NOTICE").read_bytes())


if __name__ == "__main__":
    main()
