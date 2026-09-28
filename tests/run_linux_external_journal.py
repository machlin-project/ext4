#!/usr/bin/env python3
"""Verify external-journal device pairs with a pinned native Linux reference."""

import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess

from check_namespace import inode_fields
from check_orphans import accounting
from generate_fixtures import resolve_tools


def digest(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lab", type=Path, required=True)
    parser.add_argument("--fixtures", type=Path, required=True)
    parser.add_argument("--independent", type=Path, required=True)
    parser.add_argument("--test", type=Path, required=True)
    parser.add_argument("--recover", type=Path, required=True)
    parser.add_argument("--module-report", type=Path, required=True)
    parser.add_argument("--runner", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    lab = args.lab.resolve()
    if Path.cwd() != lab:
        parser.error("run VM commands with the lab as the explicit working directory")
    root = Path(__file__).resolve().parents[1]
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    config = json.loads((lab / "config/linux-reference.json").read_text())
    modules = json.loads(args.module_report.read_text())
    source_kernel = lab / ".cache/linux-reference/vmlinuz-virt"
    if digest(source_kernel) != config["kernel"]["sha256"]:
        raise RuntimeError("Linux kernel differs from the pinned reference")
    data = source_kernel.read_bytes()
    wire = struct.Struct("<4s4sII8s32sII")
    magic, kind, offset, length, _, compression, _, _ = wire.unpack_from(data)
    if magic != b"MZ\0\0" or kind != b"zimg" or compression.rstrip(b"\0") != b"gzip" or offset < wire.size or length > len(data) - offset:
        raise RuntimeError("Invalid pinned zboot payload")
    kernel = output / "Image"
    kernel.write_bytes(gzip.decompress(data[offset:offset + length]))
    tools = resolve_tools(lab / "vendor/e2fsprogs-ext4/build")
    binaries = output / "bin"
    binaries.mkdir()
    for path in (args.test.resolve(), args.recover.resolve()):
        shutil.copy2(path, binaries / path.name)
        if not os.access(binaries / path.name, os.X_OK):
            raise RuntimeError("Prepared core executable is not executable")
    test, recover = binaries / args.test.name, binaries / args.recover.name
    protected_binaries = {str(p): digest(p) for p in (test, recover, args.runner.resolve())}
    rows = []

    def run(row, command, timeout=180):
        command = [str(part) for part in command]
        done = subprocess.run(command, cwd=lab, capture_output=True, text=True, timeout=timeout)
        row["commands"].append(dict(command=command, status=done.returncode, stdout=done.stdout, stderr=done.stderr))
        (output / "report.json").write_text(json.dumps(rows, indent=2) + "\n")
        if done.returncode:
            raise RuntimeError(f"External-journal native command failed: {command}: {done.stderr}")
        return done.stdout

    preparation = dict(kind="preparation", commands=[], binaries=protected_binaries)
    rows.append(preparation)
    run(preparation, ["/usr/bin/codesign", "--verify", "--strict", args.runner.resolve()])
    sysroot = lab / "artifacts/musl/sysroot"
    library = sysroot / "lib"
    probe = output / "init"
    linker = shutil.which("ld.lld") or str(lab / ".cache/tools/lld/bin/ld.lld")
    command = ["/opt/homebrew/opt/llvm/bin/clang", "--target=aarch64-linux-musl", f"--sysroot={sysroot}",
               f"--ld-path={linker}", "-nostdlib", "-static", "-fno-pie", "-mno-outline-atomics",
               "-O2", "-g", "-Wall", "-Wextra", "-Werror", "-Wdeclaration-after-statement",
               f"-I{root / 'include'}", f"-I{root / 'core'}", library / "crt1.o", library / "crti.o",
               root / "tests/linux_external_journal.c", f"-L{library}", "-Wl,--start-group", "-lc",
               "-lclang_rt.builtins-aarch64", "-Wl,--end-group", library / "crtn.o", "-o", probe]
    run(preparation, command)
    archives = []
    for phase in range(4):
        tree = output / f"root-{phase}"
        for name in ("dev", "mnt", "modules"):
            (tree / name).mkdir(parents=True)
        shutil.copy2(probe, tree / "init")
        (tree / "phase").write_text(f"{phase}\n")
        for name, module in modules["modules"].items():
            path = lab / module["path"]
            if digest(path) != module["sha256"]:
                raise RuntimeError(f"Pinned module changed: {path}")
            shutil.copyfile(path, tree / "modules" / f"{name}.ko")
        archive = output / f"phase-{phase}.cpio"
        listing = "\n".join(str(p.relative_to(tree)) for p in sorted(tree.rglob("*"))) + "\n"
        with archive.open("wb") as stream:
            subprocess.run(["/usr/bin/cpio", "-o", "-H", "newc"], cwd=tree,
                           input=listing.encode(), stdout=stream, check=True)
        archives.append(archive)
    preparation.update(probe_sha256=digest(probe), kernel_sha256=digest(kernel), passed=True)
    fixtures = json.loads((args.fixtures / "report.json").read_text())
    independent = {r["profile"]: r for r in json.loads((args.independent / "report.json").read_text())}
    for fixture in fixtures:
        if "profile" not in fixture or fixture["block_size"] > 4096:
            continue
        name = fixture["profile"]
        if not fixture["passed"] or not independent[name]["passed"]:
            raise RuntimeError("Native inputs lack independent acceptance")
        directory = output / name
        directory.mkdir()
        source = args.independent.resolve() / name / "state-clean.img"
        source_journal = source.with_suffix(".journal")
        pending = args.independent.resolve() / name / "state-after.img"
        pending_journal = pending.with_suffix(".journal")
        protected = {str(p): digest(p) for p in (source, source_journal, pending, pending_journal)}
        row = dict(profile=name, commands=[], protected_inputs=protected, states={})
        rows.append(row)

        def copy_pair(image, journal, stem):
            result = directory / f"{stem}.img", directory / f"{stem}.journal"
            shutil.copyfile(image, result[0])
            shutil.copyfile(journal, result[1])
            return result

        def boot(image, journal, phase):
            text = run(row, [args.runner.resolve(), kernel, archives[phase], "2", "512",
                            "console=hvc0 rdinit=/init panic=-1 loglevel=4", image, journal])
            (directory / f"phase-{phase}.console.log").write_text(text)
            for required in (f"Linux {modules['kernel_release']} aarch64",
                             "LINUX_EXTERNAL_JOURNAL_RESULT=PASS",
                             "LINUX_EXTERNAL_JOURNAL_MOUNT_OPTIONS=data=ordered,journal_path=/dev/vdb",
                             "LINUX_EXTERNAL_JOURNAL_DEVICES="):
                if required not in text:
                    raise RuntimeError(f"Missing actual guest evidence: {required}")
            for forbidden in ("EXT4-fs error", "Aborting journal", "Data will be lost", "JBD2: Detected IO errors"):
                if forbidden in text:
                    raise RuntimeError(f"Native filesystem reported: {forbidden}")

        def snapshot(image, journal, state, native):
            before = [digest(image), digest(journal)]
            run(row, [tools["e2fsck"], "-fn", "-j", journal, image])
            counts = accounting(run(row, [tools["dumpe2fs"], "-h", image]))
            inode = inode_fields(run(row, [tools["debugfs"], "-R", "stat /hello.txt", image]))
            contents = directory / f"{state}.contents"
            value = directory / f"{state}.xattr"
            run(row, [tools["debugfs"], "-R", f'dump /hello.txt "{contents}"', image])
            run(row, [tools["debugfs"], "-R", f'ea_get -r -f "{value}" /hello.txt user.transaction', image])
            if contents.read_bytes() != (b"Native ext4!\n" if native else b"Journal ext4\n") or value.read_bytes() != (b"Y" if native else b"X") * 300:
                raise RuntimeError("Native external-journal data or attribute differs")
            if inode["mode"] != (0o642 if native else 0o604):
                raise RuntimeError("Native external-journal mode differs")
            if native and (inode["uid"], inode["gid"]) != (70003, 80004):
                raise RuntimeError("Native external-journal ownership differs")
            result = dict(inode=inode, accounting=counts)
            row["states"][state] = result
            if before != [digest(image), digest(journal)]:
                raise RuntimeError("Read-only verification changed paired devices")
            return result

        image, journal = copy_pair(source, source_journal, "native")
        initial = snapshot(image, journal, "initial", False)
        boot(image, journal, 0)
        header = run(row, [tools["dumpe2fs"], "-h", image])
        if "needs_recovery" not in header:
            raise RuntimeError("Linux did not leave a pending filesystem")
        oracle_image, oracle_journal = copy_pair(image, journal, "linux-recovery")
        text = run(row, [recover, "--write", image, "--journal", journal])
        transactions = re.search(r"transactions=(\d+)", text)
        orphans = re.search(r"orphans=(\d+)", text)
        if not transactions or int(transactions[1]) == 0 or not orphans or int(orphans[1]) != 1:
            raise RuntimeError("Core did not replay native transactions and exactly one orphan")
        row.update(transactions=int(transactions[1]), cleaned_orphans=int(orphans[1]))
        recovered = snapshot(image, journal, "core-recovered", True)
        if recovered["accounting"] != initial["accounting"]:
            raise RuntimeError("Native orphan cleanup did not restore exact allocation totals")
        hashes = [digest(image), digest(journal)]
        run(row, [recover, "--write", image, "--journal", journal])
        if hashes != [digest(image), digest(journal)]:
            raise RuntimeError("Core paired-device recovery is not idempotent")
        boot(oracle_image, oracle_journal, 1)
        if snapshot(oracle_image, oracle_journal, "linux-recovered", True) != recovered:
            raise RuntimeError("Core and native Linux recovery disagree")
        replay_image, replay_journal = copy_pair(pending, pending_journal, "core-journal")
        boot(replay_image, replay_journal, 2)
        if snapshot(replay_image, replay_journal, "linux-core-replay", False) != initial:
            raise RuntimeError("Linux did not replay the core external journal exactly")
        prefix = directory / "core"
        run(row, [test, "--return", image, journal, prefix])
        returned, returned_journal = directory / "core-returned.img", directory / "core-returned.journal"
        returned_state = snapshot(returned, returned_journal, "returned", False)
        boot(returned, returned_journal, 3)
        if snapshot(returned, returned_journal, "linux-returned", False) != returned_state:
            raise RuntimeError("Linux changed the returned core filesystem")
        if any(digest(p) != sha for p, sha in protected.items()) or any(digest(p) != sha for p, sha in protected_binaries.items()):
            raise RuntimeError("Protected inputs or prepared executables changed")
        row["passed"] = True
        (output / "report.json").write_text(json.dumps(rows, indent=2) + "\n")
        print(f"PASS {name}: four native Linux boots, external journal replay, orphan cleanup and return", flush=True)


if __name__ == "__main__":
    main()
