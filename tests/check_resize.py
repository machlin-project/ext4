#!/usr/bin/env python3
"""Independently check completed and interrupted live truncate operations."""

import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess

from check_orphans import accounting, digest
from generate_fixtures import resolve_tools


def inode_fields(text):
    fields = {}
    patterns = {
        "inode": r"Inode:\s+(\d+)", "mode": r"Mode:\s+([0-7]+)",
        "size": r"(?m)^User:.*?Size:\s+(\d+)", "blocks": r"Blockcount:\s+(\d+)",
        "uid": r"User:\s+(\d+)", "gid": r"Group:\s+(\d+)",
        "links": r"Links:\s+(\d+)", "generation": r"Generation:\s+(\d+)",
        "mtime": r"(?m)^\s*mtime:\s+(0x[0-9a-f]+(?::[0-9a-f]+)?)",
        "ctime": r"(?m)^\s*ctime:\s+(0x[0-9a-f]+(?::[0-9a-f]+)?)",
    }
    for field, pattern in patterns.items():
        match = re.search(pattern, text)
        if not match:
            raise RuntimeError(f"missing independent inode field: {field}")
        fields[field] = match[1] if field in ("mtime", "ctime") else int(match[1], 8 if field == "mode" else 10)
    return fields


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exports", type=Path, required=True)
    parser.add_argument("--large", action="store_true")
    parser.add_argument("--recover", type=Path, required=True)
    parser.add_argument("--tools-root", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    tools = resolve_tools(args.tools_root)
    exports = args.exports.resolve()
    images = sorted(exports.glob("after-*.img"))
    if not images:
        raise RuntimeError("no live truncate exports")
    records = []
    for image in images:
        name = image.name.removeprefix("after-")
        unwritten = name.startswith("unwritten-")
        target = "empty" if unwritten or args.large else "payload.bin"
        before = exports / f"before-{name}"
        pending = exports / f"pending-{name}"
        protected = {p: digest(p) for p in (before, image, pending)}
        record = dict(image=str(image), input_sha256=protected[image], target=target,
                      before_image=str(before), before_sha256=protected[before],
                      pending_image=str(pending), pending_sha256=protected[pending], commands=[])
        records.append(record)

        def save():
            (output / "report.json").write_text(json.dumps(records, indent=2) + "\n")

        def run(command, allowed=(0,)):
            done = subprocess.run([str(part) for part in command], capture_output=True, text=True, timeout=90)
            record["commands"].append(dict(command=[str(part) for part in command],
                                           status=done.returncode, stdout=done.stdout, stderr=done.stderr))
            save()
            if done.returncode not in allowed:
                raise RuntimeError(f"failed ({done.returncode}): {command}")
            return done.stdout

        def inspect(candidate, label):
            run([tools["e2fsck"], "-fn", candidate])
            counts = accounting(run([tools["dumpe2fs"], "-h", candidate]))
            inode = inode_fields(run([tools["debugfs"], "-R", f"stat /{target}", candidate]))
            contents = output / f"{name}.{label}.data"
            run([tools["debugfs"], "-R", f"dump /{target} {contents}", candidate])
            if contents.stat().st_size != inode["size"]:
                raise RuntimeError("independent file dump has incorrect length")
            return counts, inode, digest(contents)

        original, old_inode, old_digest = inspect(before, "before")
        final, new_inode, new_digest = inspect(image, "after")
        block_size = final["Block size"]
        expected_data = (bytes(block_size + 7) if unwritten or args.large else
                         bytes((index * 17 + 23) & 255 for index in range(block_size + 7)))
        if (output / f"{name}.after.data").read_bytes() != expected_data:
            raise RuntimeError("truncate changed retained bytes or exposed unwritten backing data")
        for field in ("inode", "generation", "uid", "gid", "links"):
            if new_inode[field] != old_inode[field]:
                raise RuntimeError(f"truncate changed unselected field: {field}")
        if (new_inode["size"] != block_size + 7 or new_inode["mode"] != 0o640 or
                int(new_inode["mtime"].split(":")[0], 16) != 1700000301 or
                int(new_inode["ctime"].split(":")[0], 16) != 1700000302):
            raise RuntimeError("target size, permission or captured timestamps did not persist")
        if new_inode["blocks"] != (0 if args.large else 2 * block_size // 512):
            raise RuntimeError("live truncate leaked data or mapping blocks")
        freed = old_inode["blocks"] - new_inode["blocks"]
        if freed < 0 or freed % (block_size // 512):
            raise RuntimeError("invalid released-block accounting")
        expected_counts = dict(original)
        expected_counts["Free blocks"] += freed // (block_size // 512)
        if final != expected_counts:
            raise RuntimeError("released blocks or unchanged inodes do not match filesystem summaries")
        if not args.large:
            for logical in (0, 1):
                mappings = []
                for candidate in (before, image):
                    mapping = run([tools["debugfs"], "-R", f"bmap /{target} {logical}", candidate])
                    match = re.fullmatch(r"\s*(\d+)( \(uninit\))?\s*", mapping)
                    if not match or bool(match[2]) != unwritten:
                        raise RuntimeError("retained mapping changed written/unwritten state")
                    mappings.append(int(match[1]))
                if mappings[0] != mappings[1] or mappings[0] == 0:
                    raise RuntimeError("truncate moved a retained block")
                raw = []
                for candidate in (before, image):
                    with candidate.open("rb") as stream:
                        stream.seek(mappings[0] * block_size)
                        raw.append(stream.read(block_size))
                if unwritten and (not any(raw[0]) or raw[0] != raw[1]):
                    raise RuntimeError("unwritten retained backing bytes were altered")
                if not unwritten and logical == 1 and any(raw[1][7:]):
                    raise RuntimeError("written EOF tail was not zeroed")
        clean = output / f"core-{name}"
        oracle = output / f"oracle-{name}"
        shutil.copyfile(pending, clean)
        shutil.copyfile(pending, oracle)
        run([args.recover.resolve(), "--write", clean])
        core_counts, core_inode, core_digest = inspect(clean, "core")
        outcomes = {"old": (old_inode, old_digest), "new": (new_inode, new_digest)}
        outcome = next((label for label, state in outcomes.items() if state == (core_inode, core_digest)), None)
        if outcome is None or core_counts != (original if outcome == "old" else final):
            raise RuntimeError("live crash recovery returned a partial inode/attribute outcome")
        run([tools["e2fsck"], "-fy", "-E", "fixes_only", oracle], allowed=(0, 1))
        oracle_counts, oracle_inode, oracle_digest = inspect(oracle, "oracle")
        if (oracle_inode, oracle_digest) != (core_inode, core_digest):
            raise RuntimeError("portable recovery disagrees with e2fsck on the interrupted live call")
        directory_blocks = []
        for candidate in (clean, oracle):
            text = run([tools["debugfs"], "-R", "stat /many", candidate])
            directory_blocks.append(int(re.search(r"Blockcount:\s+(\d+)", text)[1]))
        delta = directory_blocks[1] - directory_blocks[0]
        if delta < 0 or delta % (block_size // 512):
            raise RuntimeError("unexpected independent directory allocation")
        oracle_counts["Free blocks"] += delta // (block_size // 512)
        if oracle_counts != core_counts:
            raise RuntimeError("e2fsck and portable recovery disagree on allocation accounting")
        for candidate in (image, clean, oracle):
            contents = output / f"{name}.{candidate.stem}.hello"
            run([tools["debugfs"], "-R", f"dump /hello-hardlink {contents}", candidate])
            if contents.read_bytes() != b"Machlin ext4\n":
                raise RuntimeError("live truncate damaged another inode")
        recovered_hash = digest(clean)
        run([args.recover.resolve(), "--write", clean])
        if digest(clean) != recovered_hash or any(digest(p) != sha for p, sha in protected.items()):
            raise RuntimeError("repeat recovery or inspection changed protected input")
        record.update(block_size=block_size, size=block_size + 7, inode=new_inode,
                      accounting=final, recovered_outcome=outcome, recovered_sha256=recovered_hash,
                      oracle_directory_blocks_added=delta // (block_size // 512), passed=True)
        save()
        print(f"PASS {name}: live truncate, retained bytes/attributes, freed blocks, independent crash recovery", flush=True)


if __name__ == "__main__":
    main()
