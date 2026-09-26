#!/usr/bin/env python3
"""Roundtrip exported pending journals through an isolated Machlin lab Linux VM."""

import argparse
import gzip
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lab", required=True, type=Path)
    parser.add_argument("--exports", required=True, type=Path)
    parser.add_argument("--module-report", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--prepare-only", action="store_true")
    args = parser.parse_args()
    lab = args.lab.resolve()
    root = Path(__file__).resolve().parent.parent
    output = args.output.resolve()
    if Path.cwd().resolve() != lab:
        parser.error("run from the explicit Machlin lab working directory")
    if output.exists():
        parser.error("output must be a new directory")
    output.mkdir(parents=True)
    module_report = json.loads(args.module_report.read_text())
    config = json.loads((lab / "config/linux-reference.json").read_text())
    kernel_source = lab / ".cache/linux-reference/vmlinuz-virt"
    if digest(kernel_source) != config["kernel"]["sha256"]:
        raise RuntimeError("kernel does not match pinned reference configuration")
    wire = struct.Struct("<4s4sII8s32sII")
    image = kernel_source.read_bytes()
    magic, kind, offset, length, _, compression, _, _ = wire.unpack_from(image)
    if magic != b"MZ\0\0" or kind != b"zimg" or compression.rstrip(b"\0") != b"gzip":
        raise RuntimeError("unexpected reference zboot format")
    if offset < wire.size or length > len(image) - offset:
        raise RuntimeError("out-of-bounds zboot payload")
    kernel = output / "Image"
    kernel.write_bytes(gzip.decompress(image[offset:offset + length]))
    runner = lab / ".cache/linux-reference/linux-vm"
    subprocess.run(["/usr/bin/codesign", "--verify", "--strict", runner], check=True)
    sysroot = lab / "artifacts/musl/sysroot"
    library = sysroot / "lib"
    probe = output / "init"
    clang = Path("/opt/homebrew/opt/llvm/bin/clang")
    linker = shutil.which("ld.lld") or str(lab / ".cache/tools/lld/bin/ld.lld")
    if not Path(linker).is_file():
        raise RuntimeError("Linux reference requires the lab's prepared ELF linker")
    command = [clang, "--target=aarch64-linux-musl", f"--sysroot={sysroot}",
               f"--ld-path={linker}", "-nostdlib", "-static", "-fno-pie",
               "-mno-outline-atomics", "-O2", "-g", "-Wall", "-Wextra", "-Werror",
               "-Wdeclaration-after-statement", f"-I{root / 'include'}", f"-I{root / 'core'}",
               library / "crt1.o", library / "crti.o", root / "tests/linux_journal_init.c",
               f"-L{library}", "-Wl,--start-group", "-lc", "-lclang_rt.builtins-aarch64",
               "-Wl,--end-group", library / "crtn.o", "-o", probe]
    with (output / "build.log").open("wb") as log:
        subprocess.run([str(x) for x in command], stdout=log, stderr=subprocess.STDOUT, check=True)
    archives = {}
    exports = sorted(args.exports.resolve().glob("writer-*.json"))
    if not exports:
        raise RuntimeError("no exported writer cases")
    for metadata in exports:
        block_size = json.loads(metadata.read_text())["block_size"]
        if block_size in archives:
            continue
        tree = output / f"root-{block_size}"
        for directory in ("dev", "mnt", "modules"):
            (tree / directory).mkdir(parents=True)
        shutil.copyfile(probe, tree / "init")
        (tree / "init").chmod(0o755)
        (tree / "block-size").write_text(f"{block_size}\n")
        for name, record in module_report["modules"].items():
            source = lab / record["path"]
            if digest(source) != record["sha256"]:
                raise RuntimeError(f"module changed: {source}")
            shutil.copyfile(source, tree / "modules" / f"{name}.ko")
        listing = "\n".join(str(path.relative_to(tree)) for path in sorted(tree.rglob("*"))) + "\n"
        archive = output / f"initramfs-{block_size}.cpio"
        with archive.open("wb") as stream:
            subprocess.run(["/usr/bin/cpio", "-o", "-H", "newc"], cwd=tree,
                           input=listing.encode(), stdout=stream, check=True)
        archives[block_size] = archive
    if args.prepare_only:
        print(f"Prepared Linux probe and {len(archives)} initramfs archives in {output}")
        return
    tools = lab / "vendor/e2fsprogs-ext4/build"
    results = []
    for metadata in exports:
        case = json.loads(metadata.read_text())
        source = metadata.with_suffix(".img")
        scratch = output / source.name
        shutil.copyfile(source, scratch)
        record = {"case": source.name, "input_sha256": digest(source), "commands": []}
        results.append(record)

        def run(command, timeout=90):
            done = subprocess.run([str(x) for x in command], cwd=lab,
                                  capture_output=True, text=True, timeout=timeout)
            record["commands"].append({"command": [str(x) for x in command],
                                       "status": done.returncode, "stdout": done.stdout,
                                       "stderr": done.stderr})
            (output / "report.json").write_text(json.dumps(results, indent=2) + "\n")
            done.check_returncode()
            return done.stdout

        console = run([runner, kernel, archives[case["block_size"]], "2", "512",
                       "console=hvc0 rdinit=/init panic=-1 loglevel=4", scratch])
        (output / f"{source.stem}.console.log").write_text(console)
        for marker in ("LINUX_EXT4_REPLAY_PASS", "LINUX_EXT4_COMMITTED_RECOVERY_PENDING",
                       "LINUX_EXT4_PROBE_RESULT=PASS", f"Linux {module_report['kernel_release']} aarch64"):
            if marker not in console:
                raise RuntimeError(f"missing guest evidence: {marker}")
        header = run([tools / "misc/dumpe2fs", "-h", scratch])
        if "needs_recovery" not in header:
            raise RuntimeError("Linux did not leave a pending journal for the reverse roundtrip")
        recovery = run([root / ".build/ext4-recover", "--write", scratch])
        transactions = re.search(r"transactions=(\d+)", recovery)
        if not transactions or int(transactions[1]) == 0:
            raise RuntimeError("reverse roundtrip did not replay a Linux-authored transaction")
        contents = output / f"{source.stem}.payload"
        run([tools / "debugfs/debugfs", "-R", f"dump /payload.bin {contents}", scratch])
        data = contents.read_bytes()
        expected = bytearray((index * 17 + 23) & 255 for index in range(200000))
        block_size = case["block_size"]
        expected[:block_size] = b"\x53" * block_size
        expected[block_size:block_size * 2] = b"\xa7" * block_size
        expected[:4] = bytes.fromhex("c03b3998")
        expected[0] = 0x6c
        if data != expected:
            raise RuntimeError("incorrect contents after Linux/native recovery roundtrip")
        status = run([tools / "debugfs/debugfs", "-R", "stat /payload.bin", scratch])
        if not re.search(r"User:\s+12345\s+Group:\s+23456", status) or not re.search(r"Mode:\s+0600", status):
            raise RuntimeError("Linux-authored ownership or mode was not preserved")
        run([tools / "e2fsck/e2fsck", "-fn", scratch])
        record["output_sha256"] = digest(scratch)
        record["passed"] = True
        (output / "report.json").write_text(json.dumps(results, indent=2) + "\n")
        print(f"PASS {source.name}: Linux replay, Linux commit, core replay, bytes/mode/owners, e2fsck", flush=True)


if __name__ == "__main__":
    main()
