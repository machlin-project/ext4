#!/usr/bin/env python3
"""Generate independent pending JBD2 logs with e2fsprogs debugfs."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

from generate_fixtures import resolve_tools

JBD2_MAGIC = 0xC03B3998
JOURNAL_PAYLOAD = "/payload.bin"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tools-root", type=Path, help="e2fsprogs build; defaults to tools on PATH")
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--checksum-v1", action="store_true",
                        help="author transaction-wide CRC32 journals on a non-metadata_csum source")
    args = parser.parse_args()
    output = args.output.resolve()
    if output.exists():
        parser.error("output must be a new directory")
    output.mkdir(parents=True)
    tools = resolve_tools(args.tools_root)
    debugfs = tools["debugfs"]
    dumpe2fs = tools["dumpe2fs"]
    source = args.source.resolve()
    transcript = []

    def run(command):
        result = subprocess.run([str(x) for x in command], capture_output=True, text=True)
        transcript.append({"command": [str(x) for x in command], "status": result.returncode,
                           "stdout": result.stdout, "stderr": result.stderr})
        (output / "commands.json").write_text(json.dumps(transcript, indent=2) + "\n")
        result.check_returncode()
        return result.stdout

    header = run([dumpe2fs, "-h", source])
    block_size = int(re.search(r"^Block size:\s+(\d+)$", header, re.M)[1])
    if args.checksum_v1 and "metadata_csum" in header:
        raise RuntimeError("checksum v1 generation requires a source without metadata_csum")
    # This fixture file has a shallow extent tree, so debugfs's block listing
    # consists of data blocks. Require enough independently reported locations.
    blocks = [int(x) for x in run([debugfs, "-R", f"blocks {JOURNAL_PAYLOAD}", source]).split()]
    if len(blocks) < 2 or len(set(blocks)) != len(blocks):
        raise RuntimeError("payload requires at least two distinct data blocks")
    targets = blocks[:2]
    original = source.read_bytes()
    old = [original[x * block_size:(x + 1) * block_size] for x in targets]
    new = [bytearray([0x53]) * block_size, bytearray([0xA7]) * block_size]
    new[0][:4] = JBD2_MAGIC.to_bytes(4, "big")
    later = bytes([0x3D]) * block_size
    data = output / "transaction.bin"
    data.write_bytes(b"".join(new))
    (output / "later.bin").write_bytes(later)
    (output / "empty.bin").write_bytes(b"")
    cases = []
    profiles = (
        ("committed-plain", 0, False, False, False),
        ("committed-v2", 2, False, False, False),
        ("committed-v3", 3, False, False, False),
        ("uncommitted-tail-v3", 3, True, False, False),
        ("revoked-v3", 3, False, True, False),
        ("rewritten-v3", 3, False, True, True),
    )
    if args.checksum_v1:
        profiles = (("committed-v1", 1, False, False, False),
                    ("uncommitted-tail-v1", 1, True, False, False),
                    ("rewritten-v1", 1, False, False, True))
    for name, version, uncommitted, revoke, rewrite in profiles:
        image = output / f"{name}.img"
        shutil.copyfile(source, image)
        # debugfs selects v1 when the filesystem has no metadata checksums.
        options = " -c" if version == 1 else f" -c -v {version}" if version else ""
        commands = [f"journal_open{options}",
                    f"journal_write -b {targets[0]},{targets[1]} {data}"]
        if uncommitted:
            commands.append(f"journal_write -c -b {targets[0]} {output / 'later.bin'}")
        if revoke:
            commands.append(f"journal_write -r {targets[0]} {output / 'empty.bin'}")
        if rewrite:
            commands.append(f"journal_write -b {targets[0]} {output / 'later.bin'}")
        commands.append("journal_close")
        script = output / f"{name}.commands"
        script.write_text("\n".join(commands) + "\n")
        run([debugfs, "-w", "-f", script, image])
        image_header = run([dumpe2fs, "-h", image])
        if version == 1:
            features = re.search(r"^Journal features:\s+(.+)$", image_header, re.M)
            if not features or "journal_checksum" not in features[1].split() or any(
                    feature in features[1].split() for feature in
                    ("journal_checksum_v2", "journal_checksum_v3", "journal_async_commit")):
                raise RuntimeError(f"{name}: debugfs did not select synchronous checksum v1")
        log = run([debugfs, "-R", "logdump -a", image])
        start = re.search(r"^Journal start:\s+(\d+)$", image_header, re.M)
        if "needs_recovery" not in image_header or not start or int(start[1]) == 0:
            raise RuntimeError(f"{name}: debugfs did not leave a pending journal")
        if "commit" not in log.lower():
            raise RuntimeError(f"{name}: independent logdump did not find a commit")
        expected = list(new)
        if revoke:
            expected[0] = old[0]
        if rewrite:
            expected[0] = later
        expected_paths = []
        for index, contents in enumerate(expected):
            path = output / f"{name}-block-{index}.bin"
            path.write_bytes(contents)
            expected_paths.append(path.name)
        cases.append({"name": name, "image": image.name, "targets": targets,
                      "expected": expected_paths, "transactions": 1 + int(revoke) + int(rewrite),
                      "image_sha256": hashlib.sha256(image.read_bytes()).hexdigest()})
    manifest = {"source": str(source), "source_sha256": hashlib.sha256(original).hexdigest(),
                "block_size": block_size, "cases": cases}
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Generated {len(cases)} independent journal cases in {output}")


if __name__ == "__main__":
    main()
