#!/usr/bin/env python3
"""Compare all six directory hash versions with independent e2fsprogs results."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import uuid

from generate_fixtures import resolve_tools


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--tools-root", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    probe = args.probe.resolve()
    tools = resolve_tools(args.tools_root)
    lengths = [1, 2, 3, 4, 5, 7, 8, 15, 16, 17, 31, 32, 33, 63, 64, 65,
               127, 128, 129, 254, 255]
    patterns = [b"abcdXYZ019-_", bytes([0x80, 0xff, 0x81, 0xfe, 0xbf, 0xc0]),
                b"bytes-" + bytes([0xe2, 0x98, 0x83, 0xf0, 0x9f, 0x92, 0xbe])]
    names = [b"hello", b".", b"..", b"a name with spaces"]
    for length in lengths:
        for pattern in patterns:
            names.append((pattern * (length // len(pattern) + 1))[:length])
    seeds = [None, "00000000-0000-0000-0000-000000000000",
             "2e3fadb8-46b1-4c67-9545-f0d317e8cd57", "ffffffff-ffff-ffff-ffff-ffffffffffff"]
    records = []
    lines = []
    report = {"probe": str(probe), "probe_sha256": hashlib.sha256(probe.read_bytes()).hexdigest(),
              "debugfs": str(tools["debugfs"]), "records": records}

    def save():
        (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")

    for version in range(6):
        for seed in seeds:
            seed_bytes = uuid.UUID(seed).bytes if seed is not None else bytes(16)
            words = [int.from_bytes(seed_bytes[index:index + 4], "little")
                     for index in range(0, 16, 4)]
            for name in names:
                # Numeric versions are intentional: debugfs silently treats an
                # unrecognized hash name as version zero. Bytes preserve non-UTF8 names.
                command = f"dx_hash -h {version}".encode()
                if seed is not None:
                    command += b" -s " + seed.encode()
                command += b' "' + name + b'"'
                done = subprocess.run([bytes(tools["debugfs"]), b"-R", command],
                                      capture_output=True, timeout=30)
                found = re.search(rb" is 0x([0-9a-f]+) \(minor 0x([0-9a-f]+)\)\n$", done.stdout)
                record = dict(version=version, seed=seed, name_hex=name.hex(),
                              command_hex=command.hex(), status=done.returncode,
                              stdout_hex=done.stdout.hex(), stderr=done.stderr.decode(errors="backslashreplace"))
                records.append(record)
                if done.returncode or found is None:
                    save()
                    raise RuntimeError("Independent hash query failed or returned unexpected output")
                record["expected"] = [int(found[1], 16), int(found[2], 16)]
                lines.append(f"{version} " + " ".join(f"{word:08x}" for word in words) + " " + name.hex())
        save()
    done = subprocess.run([probe, "--stream"], input="\n".join(lines) + "\n", text=True,
                          capture_output=True, timeout=60)
    report["probe_status"] = done.returncode
    report["probe_stderr"] = done.stderr
    output_lines = done.stdout.splitlines()
    if done.returncode or len(output_lines) != len(records):
        save()
        raise RuntimeError("Portable hash probe failed or returned the wrong number of results")
    for record, line in zip(records, output_lines):
        if re.fullmatch(r"[0-9a-f]{8} [0-9a-f]{8}", line) is None:
            save()
            raise RuntimeError("Malformed portable hash result")
        record["observed"] = [int(value, 16) for value in line.split()]
        record["passed"] = record["observed"] == record["expected"]
        if not record["passed"]:
            save()
            raise RuntimeError(f"Directory hash disagrees with e2fsprogs: {record}")
    report["passed"] = True
    save()
    print(f"PASS {len(records)} directory hashes: six versions, four seeds, byte and length variations")


if __name__ == "__main__":
    main()
