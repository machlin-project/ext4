#!/usr/bin/env python3
"""Compare paired-device transactions against nonrepairing e2fsprogs checks."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

from generate_fixtures import resolve_tools
from check_namespace import inode_fields
from check_large_volumes import accounting


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixtures", required=True, type=Path)
    parser.add_argument("--test", required=True, type=Path)
    parser.add_argument("--recover", required=True, type=Path)
    parser.add_argument("--tools-root", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    tools = resolve_tools(args.tools_root)
    fixtures = json.loads((args.fixtures / "report.json").read_text())
    rows = []
    for fixture in fixtures:
        if "profile" not in fixture:
            continue
        name = fixture["profile"]
        directory = output / name
        directory.mkdir()
        prefix = directory / "state"
        source = Path(fixture["image"])
        source_journal = Path(fixture["journal"])
        row = dict(profile=name, commands=[], states={})
        rows.append(row)

        def run(command):
            command = [str(part) for part in command]
            done = subprocess.run(command, capture_output=True, text=True, timeout=300)
            row["commands"].append(dict(command=command, status=done.returncode,
                                        stdout=done.stdout, stderr=done.stderr))
            (output / "report.json").write_text(json.dumps(rows, indent=2) + "\n")
            if done.returncode:
                raise RuntimeError(f"External-journal verification failed: {command}: {done.stderr}")
            return done.stdout

        def snapshot(image, journal, state, changed):
            before = [digest(image), digest(journal)]
            run([tools["e2fsck"], "-fn", "-j", journal, image])
            counts = accounting(run([tools["dumpe2fs"], "-h", image]))
            inode = inode_fields(run([tools["debugfs"], "-R", "stat /hello.txt", image]))
            contents = directory / f"{state}.contents"
            run([tools["debugfs"], "-R", f'dump /hello.txt "{contents}"', image])
            expected = b"Journal ext4\n" if changed else b"Machlin ext4\n"
            if contents.read_bytes() != expected or inode["size"] != len(expected):
                raise RuntimeError("External-journal recovered contents/EOF differ")
            if inode["mode"] != (0o604 if changed else 0o644):
                raise RuntimeError("External-journal transaction lost its permission change")
            if changed:
                value = directory / f"{state}.xattr"
                run([tools["debugfs"], "-R", f'ea_get -r -f "{value}" /hello.txt user.transaction', image])
                if value.read_bytes() != b"X" * 300:
                    raise RuntimeError("External-journal transaction lost its attribute value")
                if any(inode[field] != (1700000700, 0) for field in ("ctime", "mtime")):
                    raise RuntimeError("External-journal transaction lost its timestamps")
            if before != [digest(image), digest(journal)]:
                raise RuntimeError("Read-only independent checks changed a device")
            result = dict(inode=inode, accounting=counts, contents=expected.hex())
            row["states"][state] = result
            return result

        if digest(source) != fixture["image_sha256"] or digest(source_journal) != fixture["journal_sha256"]:
            raise RuntimeError("External-journal input pair changed")
        original = snapshot(source, source_journal, "original", False)
        run([args.test.resolve(), "--export", source, source_journal, prefix])
        committed = None
        for state in ("clean", "before-recovered", "after-recovered"):
            image = Path(f"{prefix}-{state}.img")
            journal = Path(f"{prefix}-{state}.journal")
            changed = state != "before-recovered"
            actual = snapshot(image, journal, state, changed)
            if changed:
                if actual["accounting"]["Free blocks"] != original["accounting"]["Free blocks"] - 1:
                    raise RuntimeError("External attribute must allocate exactly one home block")
                if actual["inode"]["blocks"] != original["inode"]["blocks"] + fixture["block_size"] // 512:
                    raise RuntimeError("External attribute sector accounting differs")
                if committed is not None and actual != committed:
                    raise RuntimeError("Committed replay differs from a completed transaction")
                committed = actual
            elif actual != original:
                raise RuntimeError("Uncommitted recovery changed the filesystem")
            hashes = [digest(image), digest(journal)]
            text = run([args.recover.resolve(), "--write", image, "--journal", journal])
            if "transactions=0 " not in text or hashes != [digest(image), digest(journal)]:
                raise RuntimeError("Repeated clean recovery changed a device")
        for state in ("before", "after"):
            image = directory / f"oracle-{state}.img"
            journal = directory / f"oracle-{state}.journal"
            shutil.copyfile(f"{prefix}-{state}.img", image)
            shutil.copyfile(f"{prefix}-{state}.journal", journal)
            # FORCE overrides journal_only in e2fsck. Omit -f and reject any
            # full filesystem pass: this oracle may replay but must not repair.
            text = run([tools["e2fsck"], "-y", "-E", "journal_only", "-j", journal, image])
            if re.search(r"^Pass [1-5]", text, re.M):
                raise RuntimeError("Journal oracle unexpectedly entered filesystem repair passes")
            actual = snapshot(image, journal, f"oracle-{state}", state == "after")
            if actual != (original if state == "before" else committed):
                raise RuntimeError("Core and independent paired-device replay disagree")
        if digest(source) != fixture["image_sha256"] or digest(source_journal) != fixture["journal_sha256"]:
            raise RuntimeError("External-journal verification changed its input pair")
        row["passed"] = True
        (output / "report.json").write_text(json.dumps(rows, indent=2) + "\n")
        print(f"PASS {name}: six independent states, paired-device replay and idempotence", flush=True)


if __name__ == "__main__":
    main()
