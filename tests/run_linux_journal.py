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

from check_allocation import expected_contents
import linux_namespace
import linux_xattrs
from sparse_image import sparse_copy, sparse_digest


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def image_digest(case, path):
    return sparse_digest(path) if case.get("large_volume") else digest(path)


def copy_image(case, source, target):
    if case.get("large_volume"):
        sparse_copy(source, target)
    else:
        shutil.copyfile(source, target)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lab", required=True, type=Path)
    parser.add_argument("--exports", required=True, type=Path)
    parser.add_argument("--module-report", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--prepare-only", action="store_true")
    parser.add_argument("--no-delalloc", action="store_true",
                        help="explicit Linux allocation control; disable delayed allocation")
    parser.add_argument("--file-writes", action="store_true",
                        help="verify clean file-write exports listed by check_writes.py")
    parser.add_argument("--allocation", action="store_true",
                        help="verify allocation exports and replay a Linux-authored file extension")
    parser.add_argument("--truncate", action="store_true",
                        help="verify truncate exports and replay Linux shrink/grow/reallocation")
    parser.add_argument("--orphans", action="store_true",
                        help="generate pending Linux orphan fixtures from checked clean exports")
    parser.add_argument("--live-truncate", action="store_true",
                        help="verify completed or interrupted live truncates and Linux regrowth")
    parser.add_argument("--namespace", action="store_true",
                        help="verify independently checked namespace exports and Linux inode reuse")
    parser.add_argument("--xattr-truncate", action="store_true",
                        help="verify Linux preserves external attributes when recovering a linked truncate")
    parser.add_argument("--xattrs", action="store_true",
                        help="verify raw attributes and ACLs through Linux/core/Linux mutation roundtrips")
    parser.add_argument("--xattr-reader", type=Path,
                        help="exact portable attribute test executable (required for --xattrs)")
    parser.add_argument("--pending", action="store_true",
                        help="with --namespace or --xattrs, mount a pending journal instead of its clean result")
    parser.add_argument("--recover", type=Path,
                        help="portable recovery executable (required for --namespace)")
    parser.add_argument("--case", action="append", default=[],
                        help="select an exact exported image filename (repeatable)")
    args = parser.parse_args()
    if sum((args.file_writes, args.allocation, args.truncate, args.orphans,
            args.live_truncate, args.namespace, args.xattr_truncate, args.xattrs)) > 1:
        parser.error("select one mutation verification mode")
    if args.pending and not (args.namespace or args.xattrs):
        parser.error("--pending requires --namespace or --xattrs")
    if (args.namespace or args.xattrs) and args.recover is None:
        parser.error("namespace/attribute modes require the exact prepared --recover executable")
    if args.xattrs and args.xattr_reader is None:
        parser.error("--xattrs requires the exact prepared --xattr-reader executable")
    clean_exports = (args.file_writes or args.allocation or args.truncate or args.orphans or
                     args.live_truncate or args.namespace or args.xattr_truncate or args.xattrs)
    lab = args.lab.resolve()
    root = Path(__file__).resolve().parent.parent
    recover = args.recover.resolve() if args.recover else root / ".build/ext4-recover"
    recover_sha = digest(recover) if not args.orphans and (args.namespace or args.xattrs or not args.prepare_only) else None
    reader = args.xattr_reader.resolve() if args.xattrs else None
    reader_sha = digest(reader) if reader else None
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
    if args.file_writes:
        command.insert(1, "-DEXT4_TEST_FILE_WRITES=1")
    if args.allocation:
        command.insert(1, "-DEXT4_TEST_ALLOCATION=1")
    if args.truncate:
        command.insert(1, "-DEXT4_TEST_TRUNCATE=1")
    if args.orphans:
        command.insert(1, "-DEXT4_TEST_ORPHANS=1")
    if args.live_truncate:
        command.insert(1, "-DEXT4_TEST_LIVE_TRUNCATE=1")
    if args.namespace:
        command.insert(1, "-DEXT4_TEST_NAMESPACE=1")
    if args.xattr_truncate:
        command.insert(1, "-DEXT4_TEST_XATTR_TRUNCATE=1")
    if args.xattrs:
        command.insert(1, "-DEXT4_TEST_XATTRS=1")
    with (output / "build.log").open("wb") as log:
        subprocess.run([str(x) for x in command], stdout=log, stderr=subprocess.STDOUT, check=True)
    (output / "probe-build.json").write_text(json.dumps(dict(
        command=[str(x) for x in command], executable=str(probe), sha256=digest(probe)), indent=2) + "\n")
    archives = {}
    if clean_exports:
        exports = json.loads(args.exports.resolve().read_text())
        if args.xattr_truncate:
            if not all(record.get("prepared_for_xattr_truncate") for record in exports):
                raise RuntimeError("xattr truncate inputs must identify prepared pending images")
        elif not all(record.get("passed") for record in exports):
            raise RuntimeError("file-write exports need successful independent checks")
    else:
        exports = [dict(json.loads(path.read_text()), image=str(path.with_suffix(".img")))
                   for path in sorted(args.exports.resolve().glob("writer-*.json"))]
    if args.case:
        names = {Path(record["image"]).name for record in exports}
        if set(args.case) - names:
            raise RuntimeError("requested cases are absent from the exports")
        exports = [record for record in exports if Path(record["image"]).name in args.case]
    if not exports:
        raise RuntimeError("no exported writer cases")
    tools = lab / "vendor/e2fsprogs-ext4/build"

    def archive_key(case):
        if args.namespace or args.xattrs:
            return Path(case["image"]).stem
        if args.live_truncate:
            return (case["block_size"], case["target"], case["inode"]["blocks"])
        if case.get("checksum_v1") or case.get("async_commit"):
            return (case["block_size"], "checksum-v1" if case.get("checksum_v1") else "checksum-modern",
                    "async" if case.get("async_commit") else "sync")
        return case["block_size"]

    def archive_tree(tree, archive):
        listing = "\n".join(str(path.relative_to(tree)) for path in sorted(tree.rglob("*"))) + "\n"
        with archive.open("wb") as stream:
            subprocess.run(["/usr/bin/cpio", "-o", "-H", "newc"], cwd=tree,
                           input=listing.encode(), stdout=stream, check=True)

    for case in exports:
        if case.get("large_volume") and (not args.xattrs or
                case.get("image_digest_format") != "ext4-test-sparse-pages-v1"):
            raise RuntimeError("High-address volumes require sparse attribute-roundtrip input records")
        if args.namespace:
            if "accounting_after" in case:
                case["accounting"] = case["accounting_after"]
            elif "verified_split" in case:
                case["accounting"] = case["verified_split"]["accounting"]
            case["block_size"] = case["accounting"]["Block size"]
            if case["block_size"] > 4096:
                raise RuntimeError("Select namespace cases supported by the 4 KiB-page reference kernel")
            if args.pending and (not case.get("pending") or case.get("recovered_outcome") != "new"):
                raise RuntimeError("Pending namespace mode requires a checked committed namespace export")
        block_size = case["block_size"]
        if (args.xattr_truncate or args.xattrs) and block_size > 4096:
            raise RuntimeError("Select xattr cases supported by the 4 KiB-page reference kernel")
        if args.xattrs and args.pending and (not case.get("pending") or case.get("recovered_outcome") != "new"):
            raise RuntimeError("Pending attribute mode requires a checked committed export")
        key = archive_key(case)
        if key in archives:
            continue
        suffix = "-".join(str(part) for part in key) if isinstance(key, tuple) else str(key)
        tree = output / f"root-{suffix}"
        for directory in ("dev", "mnt", "modules"):
            (tree / directory).mkdir(parents=True)
        shutil.copyfile(probe, tree / "init")
        (tree / "init").chmod(0o755)
        (tree / "block-size").write_text(f"{block_size}\n")
        if args.no_delalloc:
            (tree / "no-delalloc").touch()
        if case.get("checksum_v1"):
            (tree / "journal-checksum-v1").touch()
        if case.get("async_commit"):
            (tree / "journal-async-commit").touch()
        if args.live_truncate:
            empty = int(case["target"] == "empty")
            (tree / "live-truncate").write_text(f"{empty} {empty} {case['inode']['blocks']}\n")
        if args.namespace:
            linux_namespace.prepare(case, tree, tools)
        if args.xattrs:
            linux_xattrs.prepare(case, tree, tools)
        for name, record in module_report["modules"].items():
            source = lab / record["path"]
            if digest(source) != record["sha256"]:
                raise RuntimeError(f"module changed: {source}")
            shutil.copyfile(source, tree / "modules" / f"{name}.ko")
        archive = output / f"initramfs-{tree.name.removeprefix('root-')}.cpio"
        archive_tree(tree, archive)
        archives[key] = archive
    if args.prepare_only:
        print(f"Prepared Linux probe and {len(archives)} initramfs archives in {output}")
        return
    results = []
    for case in exports:
        source = Path(case["pending"] if args.pending else case["image"])
        expected_sha = case["pending_sha256"] if args.pending else case.get("input_sha256")
        if clean_exports and image_digest(case, source) != expected_sha:
            raise RuntimeError(f"file-write export changed: {source}")
        scratch = output / source.name
        copy_image(case, source, scratch)
        record = {"case": source.name, "input_sha256": image_digest(case, source), "commands": [],
                  "linux_no_delalloc": args.no_delalloc}
        if case.get("large_volume"):
            record["image_digest_format"] = case["image_digest_format"]
        if not args.orphans:
            record.update(recover=str(recover), recover_sha256=recover_sha)
        if args.xattrs:
            record.update(attribute_reader=str(reader), attribute_reader_sha256=reader_sha)
        results.append(record)

        def run(command, timeout=90, allowed=(0,), raw=False):
            done = subprocess.run([str(x) for x in command], cwd=lab,
                                  capture_output=True, timeout=timeout)
            stdout = done.stdout.decode("utf-8", "backslashreplace")
            stderr = done.stderr.decode("utf-8", "backslashreplace")
            record["commands"].append({"command": [str(x) for x in command],
                                       "status": done.returncode, "stdout": stdout,
                                       "stderr": stderr})
            (output / "report.json").write_text(json.dumps(results, indent=2) + "\n")
            if done.returncode not in allowed:
                raise subprocess.CalledProcessError(done.returncode, command, stdout, stderr)
            return done.stdout if raw else stdout

        key = archive_key(case)
        console = run([runner, kernel, archives[key], "2", "512",
                       "console=hvc0 rdinit=/init panic=-1 loglevel=4", scratch])
        (output / f"{source.stem}.console.log").write_text(console)
        if args.no_delalloc and not re.search(r"LINUX_EXT4_MOUNT_OPTIONS=[^\n]*,nodelalloc(?:\r?\n)", console):
            raise RuntimeError("Linux did not identify the requested nodelalloc control")
        if any(message in console for message in (
                "Delayed block allocation failed", "Data will be lost", "EXT4-fs error",
                "JBD2: Detected IO errors", "Aborting journal")):
            raise RuntimeError("Linux reported a filesystem or journal failure")
        if args.xattr_truncate:
            for marker in ("LINUX_EXT4_XATTR_TRUNCATE_PASS", "LINUX_EXT4_PROBE_RESULT=PASS",
                           f"Linux {module_report['kernel_release']} aarch64"):
                if marker not in console:
                    raise RuntimeError(f"missing attributed truncate evidence: {marker}")
            run([tools / "e2fsck/e2fsck", "-fn", scratch])
            clean_sha = digest(scratch)
            if digest(recover) != recover_sha:
                raise RuntimeError("Recovery executable changed during Linux verification")
            run([recover, "--write", scratch])
            if digest(scratch) != clean_sha or digest(source) != expected_sha:
                raise RuntimeError("Clean recovery or Linux verification changed a protected image")
            record.update(image=str(scratch), output_sha256=clean_sha,
                          linux_linked_truncate_xattrs_preserved=True, passed=True)
            (output / "report.json").write_text(json.dumps(results, indent=2) + "\n")
            print(f"PASS {source.name}: Linux linked truncate, attributes, shared owner, clean unmount and e2fsck", flush=True)
            continue
        if args.orphans:
            for marker in ("LINUX_EXT4_ORPHANS_PENDING", "LINUX_EXT4_COMMITTED_RECOVERY_PENDING",
                           "LINUX_EXT4_PROBE_RESULT=PASS", f"Linux {module_report['kernel_release']} aarch64"):
                if marker not in console:
                    raise RuntimeError(f"missing orphan fixture evidence: {marker}")
            entries = re.findall(r"LINUX_EXT4_ORPHAN index=(\d+) inode=(\d+) mode=([0-7]+) size=(\d+) blocks=(\d+)", console)
            if len(entries) != 6 or {int(x[0]) for x in entries} != set(range(6)):
                raise RuntimeError("incomplete Linux orphan profile")
            record.update(image=str(scratch), block_size=case["block_size"], source_image=str(source),
                          output_sha256=digest(scratch), generated=True,
                          orphans=[dict(index=int(x[0]), inode=int(x[1]), mode=int(x[2], 8),
                                        size=int(x[3]), blocks_512=int(x[4])) for x in entries])
            (output / "report.json").write_text(json.dumps(results, indent=2) + "\n")
            print(f"GENERATED {source.name}: six Linux open-unlinked inode types; recovery not yet checked", flush=True)
            continue
        verification_marker = ("LINUX_EXT4_XATTR_PASS" if args.xattrs else
                               "LINUX_EXT4_NAMESPACE_PASS" if args.namespace else
                               "LINUX_EXT4_LIVE_TRUNCATE_PASS" if args.live_truncate else
                               "LINUX_EXT4_TRUNCATE_PASS" if args.truncate else
                               "LINUX_EXT4_ALLOCATION_PASS" if args.allocation else
                               "LINUX_EXT4_FILE_WRITE_PASS" if args.file_writes else "LINUX_EXT4_REPLAY_PASS")
        for marker in (verification_marker,
                       "LINUX_EXT4_COMMITTED_RECOVERY_PENDING",
                       "LINUX_EXT4_PROBE_RESULT=PASS", f"Linux {module_report['kernel_release']} aarch64"):
            if marker not in console:
                raise RuntimeError(f"missing guest evidence: {marker}")
        if case.get("ea_inode") and "LINUX_EXT4_EA_INODE_PASS" not in console:
            raise RuntimeError("Missing Linux EA_INODE mutation evidence")
        if case.get("inline_data") and "LINUX_EXT4_INLINE_PASS" not in console:
            raise RuntimeError("Missing Linux inline mutation evidence")
        if case.get("clustered") and "LINUX_EXT4_CLUSTER_PASS" not in console:
            raise RuntimeError("Missing Linux clustered allocation evidence")
        if case.get("large_files") and "LINUX_EXT4_LARGE_FILE_PASS" not in console:
            raise RuntimeError("Missing Linux high-offset mutation evidence")
        if case.get("large_volume") and "LINUX_EXT4_LARGE_VOLUME_PASS" not in console:
            raise RuntimeError("Missing Linux high physical-address mutation evidence")
        if case.get("large_files", {}).get("profile") in ("inline", "cluster-inline"):
            if "LINUX_EXT4_INLINE_HIGH_OFFSET_REJECTED_BEFORE_CONVERSION" not in console:
                raise RuntimeError("Missing pinned Linux inline-conversion boundary evidence")
        header = run([tools / "misc/dumpe2fs", "-h", scratch])
        if "needs_recovery" not in header:
            raise RuntimeError("Linux did not leave a pending journal for the reverse roundtrip")
        if case.get("checksum_v1"):
            features = re.search(r"^Journal features:\s+(.+)$", header, re.M)
            if not features or "journal_checksum" not in features[1].split() or any(
                    feature in features[1].split() for feature in
                    ("journal_checksum_v2", "journal_checksum_v3")):
                raise RuntimeError("Linux did not retain journal checksum v1")
            record["journal_checksum_v1"] = True
        if case.get("async_commit"):
            if "LINUX_EXT4_MOUNT_OPTIONS=data=writeback,journal_async_commit" not in console:
                raise RuntimeError("Linux did not identify the prepared async/revoke mount profile")
            features = re.search(r"^Journal features:\s+(.+)$", header, re.M)
            if not features or "journal_async_commit" not in features[1].split():
                raise RuntimeError("Linux did not retain asynchronous journal commits")
            record["journal_async_commit"] = True
        if case.get("checksum_v1") or case.get("async_commit"):
            features = re.search(r"^Journal features:\s+(.+)$", header, re.M)
            if not features or ("journal_async_commit" in features[1].split()) != bool(case.get("async_commit")):
                raise RuntimeError("Linux changed the requested journal commit mode")
            if "LINUX_EXT4_CHECKSUM_REVOKE_PASS" not in console:
                raise RuntimeError("Linux did not complete the committed directory release")
            journal_log = run([tools / "debugfs/debugfs", "-R", "logdump -a", scratch])
            if "Revoke FS block " not in journal_log:
                raise RuntimeError("Linux journal has no independently decoded revoke record")
            record["linux_revoke_records"] = journal_log.count("Revoke FS block ")
        if digest(recover) != recover_sha:
            raise RuntimeError("Recovery executable changed during the roundtrip")
        if args.xattrs:
            if digest(reader) != reader_sha:
                raise RuntimeError("Attribute executable changed during the roundtrip")

            def verify_native_attributes(candidate, expected, label):
                tree = output / f"root-{label}-{Path(case['image']).name}"
                original_tree = output / f"root-{archive_key(case)}"
                shutil.copytree(original_tree, tree)
                shutil.rmtree(tree / "expected")
                expected_case = dict(image=str(expected), input_sha256=image_digest(case, expected))
                if case.get("large_files"):
                    expected_case.update(large_files=case["large_files"],
                                         large_phase="linux" if label == "replay" else "returned")
                if case.get("large_volume"):
                    expected_case.update(large_volume=case["large_volume"],
                                         volume_phase={"replay": "linux", "returned": "returned",
                                                       "core-journal": "committed"}[label])
                linux_xattrs.prepare(expected_case, tree, tools, verify_only=True)
                archive = output / f"initramfs-{tree.name}.cpio"
                archive_tree(tree, archive)
                text = run([runner, kernel, archive, "2", "512",
                            "console=hvc0 rdinit=/init panic=-1 loglevel=4", candidate])
                (output / f"{source.stem}-{label}.console.log").write_text(text)
                if args.no_delalloc and not re.search(r"LINUX_EXT4_MOUNT_OPTIONS=[^\n]*,nodelalloc(?:\r?\n)", text):
                    raise RuntimeError(f"Linux {label} did not identify the requested nodelalloc control")
                for marker in ("LINUX_EXT4_XATTR_RETURN_PASS", "LINUX_EXT4_PROBE_RESULT=PASS",
                               f"Linux {module_report['kernel_release']} aarch64"):
                    if marker not in text:
                        raise RuntimeError(f"Missing Linux {label} attribute evidence: {marker}")
                if case.get("large_volume") and "LINUX_EXT4_LARGE_VOLUME_RETURN_PASS" not in text:
                    raise RuntimeError(f"Missing Linux {label} high-address evidence")
                if any(message in text for message in ("EXT4-fs error", "Aborting journal",
                                                       "JBD2: Detected IO errors")):
                    raise RuntimeError(f"Linux {label} reported a filesystem failure")

            checked = linux_xattrs.verify(case, scratch, output, tools, recover, reader, run,
                                         native_replay=verify_native_attributes)
            returned = Path(checked["returned_image"])
            verify_native_attributes(returned, returned, "returned")
            final = linux_xattrs.snapshot(returned, output / f"final-{source.stem}-checked", tools, run,
                                         large_file_case=dict(case, large_phase="returned") if case.get("large_files") else None,
                                         large_volume_case=dict(case, volume_phase="returned") if case.get("large_volume") else None)
            if final != checked["returned_state"]:
                raise RuntimeError("Linux verification changed the returned inode/attribute state")
            if image_digest(case, source) != expected_sha or digest(reader) != reader_sha or digest(recover) != recover_sha:
                raise RuntimeError("Attribute roundtrip changed a protected source or executable")
            record.update(checked, returned_sha256=image_digest(case, returned), linux_return_verified=True, passed=True)
            (output / "report.json").write_text(json.dumps(results, indent=2) + "\n")
            print(f"PASS {source.name}: Linux attributes/ACLs, core/oracle replay, core mutation and Linux return", flush=True)
            continue
        if args.namespace:
            if case.get("verified_flags") and "LINUX_EXT4_INODE_FLAGS_PASS" not in console:
                raise RuntimeError("Missing Linux inode flag policy evidence")
            if case.get("capacity_state") and any(message in console for message in (
                    "Delayed block allocation failed", "Data will be lost", "EXT4-fs error")):
                raise RuntimeError("Linux reported a full-disk allocation or write failure")
            if case.get("capacity_state") and "LINUX_EXT4_PREALLOCATED_FULL_WRITE_PASS" not in console:
                raise RuntimeError("Missing Linux preallocated write at zero free blocks")
            if case.get("verified_range") and "LINUX_EXT4_FILE_RANGES_PASS" not in console:
                raise RuntimeError("Missing Linux preallocation and hole-punch evidence")
            if linux_namespace.special_case(case) and "LINUX_EXT4_SPECIAL_FILES_PASS" not in console:
                raise RuntimeError("Missing Linux special-file creation and atomic whiteout evidence")
            if Path(case["image"]).name.startswith("exhaust-") and "LINUX_EXT4_NAMESPACE_REUSE_PASS" not in console:
                raise RuntimeError("Missing Linux reuse evidence after inode exhaustion")
            if case.get("verified_space") is not None and "LINUX_EXT4_FULL_BLOCKS_PASS" not in console:
                raise RuntimeError("Missing Linux full-block ENOSPC, overwrite and reuse evidence")
            record.update(linux_namespace.verify(case, scratch, output, tools, recover, run))
            if digest(source) != expected_sha:
                raise RuntimeError("Namespace roundtrip changed its protected source image")
            record.update(output_sha256=digest(scratch), passed=True)
            (output / "report.json").write_text(json.dumps(results, indent=2) + "\n")
            print(f"PASS {source.name}: Linux namespace, Linux commit, core/oracle replay, e2fsck", flush=True)
            continue
        oracle = None
        if case.get("checksum_v1") or case.get("async_commit"):
            oracle = output / f"oracle-{source.name}"
            shutil.copyfile(scratch, oracle)
            record["linux_pending_sha256"] = digest(scratch)
            run([tools / "e2fsck/e2fsck", "-fy", "-E", "journal_only", oracle])
            run([tools / "e2fsck/e2fsck", "-fn", oracle])
        recovery = run([recover, "--write", scratch])
        transactions = re.search(r"transactions=(\d+)", recovery)
        if not transactions or int(transactions[1]) == 0:
            raise RuntimeError("reverse roundtrip did not replay a Linux-authored transaction")
        name = case["target"] if args.live_truncate else ("empty" if args.allocation or args.truncate else "payload.bin")
        contents = output / f"{source.stem}.contents"
        run([tools / "debugfs/debugfs", "-R", f"dump /{name} {contents}", scratch])
        data = contents.read_bytes()
        expected = bytearray((index * 17 + 23) & 255 for index in range(200000))
        block_size = case["block_size"]
        if args.live_truncate:
            expected = bytearray(block_size * 6 + 13)
            if name == "payload.bin":
                expected[:block_size + 7] = bytes((index * 17 + 23) & 255 for index in range(block_size + 7))
            expected[block_size * 4 + 7] = 0x6c
        elif args.truncate:
            expected = bytearray(block_size * 12 + 17)
            expected[block_size * 10 + 7] = 0x6c
        elif args.allocation:
            expected = expected_contents(block_size)
            expected += bytes(block_size + 7) + b"\x6c"
        elif args.file_writes:
            expected[block_size - 7:block_size * 2 + 16] = bytes(
                (index * 29 + 7) & 255 for index in range(block_size + 23))
        else:
            expected[:block_size] = b"\x53" * block_size
            expected[block_size:block_size * 2] = b"\xa7" * block_size
            expected[:4] = bytes.fromhex("c03b3998")
        if not args.allocation and not args.truncate and not args.live_truncate:
            expected[0] = 0x6c
        if data != expected:
            raise RuntimeError("incorrect contents after Linux/native recovery roundtrip")
        status = run([tools / "debugfs/debugfs", "-R", f"stat /{name}", scratch])
        if not re.search(r"User:\s+12345\s+Group:\s+23456", status) or not re.search(r"Mode:\s+0600", status):
            raise RuntimeError("Linux-authored ownership or mode was not preserved")
        if oracle:
            oracle_contents = output / f"oracle-{source.stem}.contents"
            run([tools / "debugfs/debugfs", "-R", f"dump /{name} {oracle_contents}", oracle])
            oracle_status = run([tools / "debugfs/debugfs", "-R", f"stat /{name}", oracle])
            if oracle_contents.read_bytes() != data or oracle_status != status:
                raise RuntimeError("core and independent replay disagree on Linux data/metadata")
            before = digest(scratch)
            run([recover, "--write", scratch])
            if digest(scratch) != before:
                raise RuntimeError("repeated recovery changed the clean Linux image")
            record.update(oracle_sha256=digest(oracle), independent_replay=True)
        run([tools / "e2fsck/e2fsck", "-fn", scratch])
        if digest(source) != record["input_sha256"] or digest(recover) != recover_sha:
            raise RuntimeError("Linux roundtrip changed its protected input or recovery executable")
        record["output_sha256"] = digest(scratch)
        record["passed"] = True
        (output / "report.json").write_text(json.dumps(results, indent=2) + "\n")
        print(f"PASS {source.name}: Linux verification, Linux commit, core replay, bytes/mode/owners, e2fsck", flush=True)


if __name__ == "__main__":
    main()
