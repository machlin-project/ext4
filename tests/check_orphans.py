#!/usr/bin/env python3
"""Compare linked-truncate and Linux open-unlinked cleanup with e2fsck recovery."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess

from generate_fixtures import resolve_tools

ORPHAN_TAIL = struct.Struct("<II")
ORPHAN_MAGIC = 0x0B10CA04


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def accounting(header):
    fields = {}
    for field in ("Free blocks", "Free inodes", "Block count", "Inode count", "Block size"):
        match = re.search(rf"^{field}:\s+(\d+)$", header, re.M)
        if not match:
            raise RuntimeError(f"missing independent accounting: {field}")
        fields[field] = int(match[1])
    if ("needs_recovery" in header or "orphan_present" in header or
            not re.search(r"^Filesystem state:\s+clean$", header, re.M)):
        raise RuntimeError("orphan cleanup did not cleanly finish")
    if re.search(r"^First orphan inode:", header, re.M):
        raise RuntimeError("orphan list was not emptied")
    return fields


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    inputs = parser.add_mutually_exclusive_group(required=True)
    inputs.add_argument("--fixtures", type=Path, help="Linux generation report.json")
    inputs.add_argument("--linked-exports", type=Path, help="paired pending/clean linked truncate exports")
    parser.add_argument("--recover", type=Path, required=True)
    parser.add_argument("--tools-root", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    tools = resolve_tools(args.tools_root)
    linked = args.linked_exports is not None
    if linked:
        fixtures = [dict(case=p.name, image=str(p), output_sha256=digest(p),
                         clean=str(p.with_name(p.name.replace("pending-", "clean-", 1))))
                    for p in sorted(args.linked_exports.resolve().glob("pending-*.img"))]
    else:
        fixtures = json.loads(args.fixtures.read_text())
    if not fixtures or (not linked and not all(case.get("generated") and len(case["orphans"]) == 6 for case in fixtures)):
        raise RuntimeError("expected independently generated six-inode orphan fixtures")
    results = []
    for case in fixtures:
        source = Path(case["image"])
        if digest(source) != case["output_sha256"]:
            raise RuntimeError(f"pending fixture changed: {source}")
        image = output / source.name
        oracle = output / f"oracle-{source.name}"
        shutil.copyfile(source, image)
        shutil.copyfile(source, oracle)
        record = dict(case=case["case"], image=str(image), input_sha256=case["output_sha256"], commands=[])
        results.append(record)

        def save():
            (output / "report.json").write_text(json.dumps(results, indent=2) + "\n")

        def run(command, allowed=(0,)):
            done = subprocess.run([str(x) for x in command], capture_output=True, text=True, timeout=90)
            record["commands"].append(dict(command=[str(x) for x in command], status=done.returncode,
                                           stdout=done.stdout, stderr=done.stderr))
            save()
            if done.returncode not in allowed:
                raise RuntimeError(f"failed ({done.returncode}): {command}")
            return done.stdout

        def orphan_file(candidate, label, empty):
            header = run([tools["dumpe2fs"], "-h", candidate])
            match = re.search(r"^Orphan file inode:\s+(\d+)$", header, re.M)
            if not match:
                if "orphan_file" in header:
                    raise RuntimeError("orphan-file feature has no independently reported inode")
                return None
            number = int(match[1])
            block_size = int(re.search(r"^Block size:\s+(\d+)$", header, re.M)[1])
            status = run([tools["debugfs"], "-R", f"stat <{number}>", candidate])
            size = int(re.search(r"\bSize:\s+(\d+)", status)[1])
            generation = int(re.search(r"\bGeneration:\s+(\d+)", status)[1])
            sectors = int(re.search(r"Blockcount:\s+(\d+)", status)[1])
            if size == 0 or size % block_size or size // block_size > 512:
                raise RuntimeError("unexpected independent orphan-file size")
            mappings = []
            for logical in range(size // block_size):
                text = run([tools["debugfs"], "-R", f"bmap <{number}> {logical}", candidate])
                mapping = re.fullmatch(r"\s*(\d+)\s*", text)
                if not mapping or int(mapping[1]) == 0:
                    raise RuntimeError("orphan file acquired a hole or unwritten mapping")
                mappings.append(int(mapping[1]))
            contents = output / f"{image.stem}.{label}.orphan-file"
            run([tools["debugfs"], "-R", f"dump <{number}> {contents}", candidate])
            data = contents.read_bytes()
            if len(data) != size:
                raise RuntimeError("independent orphan-file dump has wrong size")
            for logical in range(size // block_size):
                block = data[logical * block_size:(logical + 1) * block_size]
                magic, _checksum = ORPHAN_TAIL.unpack(block[-ORPHAN_TAIL.size:])
                if magic != ORPHAN_MAGIC or (empty and any(block[:-ORPHAN_TAIL.size])):
                    raise RuntimeError("orphan cleanup left entries or damaged a block tail")
            return dict(inode=number, generation=generation, size=size, sectors=sectors,
                        mappings=mappings, content_sha256=digest(contents))

        pending_file = orphan_file(source, "pending", False)
        if linked:
            header = run([tools["dumpe2fs"], "-h", image])
            case["block_size"] = int(re.search(r"^Block size:\s+(\d+)$", header, re.M)[1])
            case["orphans"] = []
            for name, links in (("payload.bin", 1), ("hello.txt", 2)):
                status = run([tools["debugfs"], "-R", f"stat /{name}", image])
                case["orphans"].append(dict(inode=int(re.search(r"Inode:\s+(\d+)", status)[1]), links=links))
        recovery = run([args.recover.resolve(), "--write", image])
        orphans = re.search(r"orphans=(\d+)", recovery)
        if not orphans or int(orphans[1]) != len(case["orphans"]):
            raise RuntimeError("portable recovery did not clean every pending orphan")
        transfers = re.search(r"orphan_file_transfers=(\d+)", recovery)
        if pending_file and (not transfers or (not linked and int(transfers[1]) != len(case["orphans"]))):
            raise RuntimeError("Linux orphan-file entries were not independently identified in recovery")
        if linked and digest(image) != digest(Path(case["clean"])):
            raise RuntimeError("POSIX and modeled cleanup exports differ")
        run([tools["e2fsck"], "-fn", image])
        # Keep extent optimization out of the comparison. Some e2fsck versions
        # still index /many with fixes_only; measure that allocation explicitly.
        run([tools["e2fsck"], "-fy", "-E", "fixes_only", oracle], allowed=(0, 1))
        run([tools["e2fsck"], "-fn", oracle])
        observed = accounting(run([tools["dumpe2fs"], "-h", image]))
        expected = accounting(run([tools["dumpe2fs"], "-h", oracle]))
        if not linked:
            baseline = Path(case["source_image"])
            if digest(baseline) != case["input_sha256"]:
                raise RuntimeError("clean fixture baseline changed")
            original = accounting(run([tools["dumpe2fs"], "-h", baseline]))
            if observed != original:
                raise RuntimeError("creating and unlinking six objects did not return baseline space")
        directory_blocks = []
        for candidate in (image, oracle):
            status = run([tools["debugfs"], "-R", "stat /many", candidate])
            directory_blocks.append(int(re.search(r"Blockcount:\s+(\d+)", status)[1]))
        sectors_per_block = case["block_size"] // 512
        directory_delta = directory_blocks[1] - directory_blocks[0]
        if directory_delta < 0 or directory_delta % sectors_per_block:
            raise RuntimeError("unexpected oracle directory allocation")
        record["oracle_directory_blocks_added"] = directory_delta // sectors_per_block
        expected["Free blocks"] += record["oracle_directory_blocks_added"]
        if observed != expected:
            raise RuntimeError(f"portable cleanup leaked blocks/inodes: {observed} != {expected}")
        if pending_file:
            core_file = orphan_file(image, "core", True)
            oracle_file = orphan_file(oracle, "oracle", True)
            if core_file != oracle_file or any(core_file[key] != value for key, value in pending_file.items()
                                               if key != "content_sha256"):
                raise RuntimeError("portable and independent orphan-file recovery disagree")
            record["orphan_file"] = core_file
            record["orphan_file_transfers"] = int(transfers[1])
        for entry in case["orphans"]:
            for candidate in (image, oracle):
                text = run([tools["debugfs"], "-R", f"testi <{entry['inode']}>", candidate])
                state = "marked in use" if linked else "not in use"
                if not re.search(rf"Inode {entry['inode']} is {state}", text):
                    raise RuntimeError(f"incorrect recovered inode allocation: {entry['inode']}")
        payload_size = case["block_size"] + 7 if linked else 200000
        files = [("payload.bin", bytes((index * 17 + 23) & 255 for index in range(payload_size))),
                 ("hello.txt", b"Machlin" if linked else b"Machlin ext4\n")]
        if not linked:
            # Linux's orphan probe leaves this existing file alone. Different
            # independently checked input profiles may already have grown it.
            original_empty = output / f"{image.stem}.baseline.empty"
            run([tools["debugfs"], "-R", f"dump /empty {original_empty}", baseline])
            files.append(("empty", original_empty.read_bytes()))
        for name, expected_data in files:
            for label, candidate in (("core", image), ("oracle", oracle)):
                contents = output / f"{image.stem}.{label}.{name}"
                run([tools["debugfs"], "-R", f"dump /{name} {contents}", candidate])
                if contents.read_bytes() != expected_data:
                    raise RuntimeError(f"orphan cleanup damaged a live file: {name}")
        before = digest(image)
        run([args.recover.resolve(), "--write", image])
        if digest(image) != before or digest(source) != case["output_sha256"]:
            raise RuntimeError("repeated recovery or independent checks changed a protected input")
        record.update(accounting=observed, recovered_sha256=before, passed=True)
        save()
        kind = "two retained linked inodes" if linked else "six reclaimed inodes"
        print(f"PASS {source.name}: {kind}, live bytes, e2fsck oracle, repeat recovery", flush=True)


if __name__ == "__main__":
    main()
