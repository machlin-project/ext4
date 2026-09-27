#!/usr/bin/env python3
"""Find byte-name hash collisions and confirm them with independent debugfs."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import uuid

from generate_fixtures import UUID, resolve_tools

NAME_MAX = 255
CANDIDATES = 262144


def filename(index):
    name = bytearray(f"hash-{index:08x}-".encode().ljust(NAME_MAX, b"n"))
    name[16] = 0x80 + index % 128
    return bytes(name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--tools-root", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    tools = resolve_tools(args.tools_root)
    probe = args.probe.resolve()
    seed = uuid.UUID(UUID).bytes
    words = " ".join(f"{int.from_bytes(seed[index:index + 4], 'little'):08x}"
                     for index in range(0, len(seed), 4))
    report = dict(probe=str(probe), probe_sha256=hashlib.sha256(probe.read_bytes()).hexdigest(),
                  seed=UUID, candidates=CANDIDATES, records=[])
    vectors = []

    def save():
        (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")

    for version in range(6):
        lines = [f"{version} {words} {filename(index).hex()}" for index in range(CANDIDATES)]
        done = subprocess.run([probe, "--stream"], input="\n".join(lines) + "\n", text=True,
                              capture_output=True, timeout=180)
        hashes = done.stdout.splitlines()
        if done.returncode or len(hashes) != CANDIDATES:
            save()
            raise RuntimeError("Hash probe failed while finding collision candidates")
        seen = {}
        pair = None
        for index, line in enumerate(hashes):
            if re.fullmatch(r"[0-9a-f]{8} [0-9a-f]{8}", line) is None:
                raise RuntimeError("Malformed hash probe result")
            major, minor = (int(value, 16) for value in line.split())
            # Leave room on both sides for names that force a split at the collision.
            if major < 0x01000000 or major >= 0xff000000:
                continue
            if major in seen:
                pair = (seen[major], index)
                break
            seen[major] = index
        if pair is None:
            save()
            raise RuntimeError(f"No collision within the bounded search for version {version}")
        record = dict(version=version, major=major, names=[], commands=[])
        report["records"].append(record)
        for ordinal in pair:
            name = filename(ordinal)
            command = f"dx_hash -h {version} -s {UUID} ".encode() + b'"' + name + b'"'
            oracle = subprocess.run([bytes(tools["debugfs"]), b"-R", command],
                                    capture_output=True, timeout=30)
            found = re.search(rb" is 0x([0-9a-f]+) \(minor 0x([0-9a-f]+)\)\n$", oracle.stdout)
            record["commands"].append(dict(command_hex=command.hex(), status=oracle.returncode,
                                            stdout_hex=oracle.stdout.hex(), stderr_hex=oracle.stderr.hex()))
            expected = [int(value, 16) for value in hashes[ordinal].split()]
            if oracle.returncode or found is None or [int(found[1], 16), int(found[2], 16)] != expected:
                save()
                raise RuntimeError("Collision name does not agree with independent debugfs")
            record["names"].append(dict(ordinal=ordinal, name_hex=name.hex(), major=expected[0], minor=expected[1]))
        record["passed"] = True
        vectors.append(f"{version} {major:08x} " + " ".join(item["name_hex"] for item in record["names"]))
        save()
        print(f"PASS version={version} collision=0x{major:08x} ordinals={pair}", flush=True)
    (output / "vectors.txt").write_text("\n".join(vectors) + "\n")
    report["passed"] = True
    save()


if __name__ == "__main__":
    main()
