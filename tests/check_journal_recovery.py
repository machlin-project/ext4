#!/usr/bin/env python3
"""Check debugfs-generated transactions with our replayer and independent e2fsck."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    inputs = parser.add_mutually_exclusive_group(required=True)
    inputs.add_argument("--fixtures", type=Path)
    inputs.add_argument("--writers", type=Path, help="pending transactions exported by ext4-journal-test")
    parser.add_argument("--recover", required=True, type=Path)
    parser.add_argument("--e2fsck", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    if output.exists():
        parser.error("output must be a new directory")
    output.mkdir(parents=True)
    fixtures = (args.fixtures or args.writers).resolve()
    if args.fixtures:
        manifest = json.loads((fixtures / "manifest.json").read_text())
        cases = manifest["cases"]
        for case in cases:
            case["block_size"] = manifest["block_size"]
            case["expected_bytes"] = [(fixtures / path).read_bytes() for path in case["expected"]]
    else:
        cases = []
        for path in sorted(fixtures.glob("writer-*.json")):
            case = json.loads(path.read_text())
            block_size = case["block_size"]
            expected = [bytearray([0x53]) * block_size, bytes([0xA7]) * block_size]
            expected[0][:4] = bytes.fromhex("c03b3998")
            case.update(name=path.stem, image=path.with_suffix(".img").name,
                        image_sha256=digest(path.with_suffix(".img")),
                        transactions=1, expected_bytes=expected)
            cases.append(case)
    if not cases:
        raise RuntimeError("no journal cases")
    results = []
    for case in cases:
        source = fixtures / case["image"]
        if digest(source) != case["image_sha256"]:
            raise RuntimeError(f"fixture changed: {source}")
        image = output / source.name
        shutil.copyfile(source, image)
        result = {"name": case["name"], "input_sha256": case["image_sha256"], "commands": []}
        results.append(result)

        def run(command):
            done = subprocess.run([str(x) for x in command], capture_output=True, text=True)
            result["commands"].append({"command": [str(x) for x in command],
                                       "status": done.returncode, "stdout": done.stdout,
                                       "stderr": done.stderr})
            (output / "report.json").write_text(json.dumps(results, indent=2) + "\n")
            done.check_returncode()
            return done.stdout

        log = run([args.recover.resolve(), "--write", image])
        match = re.search(r"transactions=(\d+)", log)
        if not match or int(match[1]) != case["transactions"]:
            raise RuntimeError(f"incorrect recovered transaction count: {case['name']}")
        def check_bytes(candidate):
            with candidate.open("rb") as file:
                for block, expected in zip(case["targets"], case["expected_bytes"], strict=True):
                    file.seek(block * case["block_size"])
                    if file.read(case["block_size"]) != expected:
                        raise RuntimeError(f"incorrect recovered bytes: {candidate.name}, block {block}")

        check_bytes(image)
        before = digest(image)
        run([args.recover.resolve(), "--write", image])
        if digest(image) != before:
            raise RuntimeError("repeated recovery changed an already clean image")
        run([args.e2fsck.resolve(), "-fn", image])
        oracle = output / f"oracle-{source.name}"
        shutil.copyfile(source, oracle)
        run([args.e2fsck.resolve(), "-fy", "-E", "journal_only", oracle])
        check_bytes(oracle)
        run([args.e2fsck.resolve(), "-fn", oracle])
        if digest(source) != case["image_sha256"]:
            raise RuntimeError("journal verification modified its source")
        result["recovered_sha256"] = before
        result["oracle_sha256"] = digest(oracle)
        result["passed"] = True
        (output / "report.json").write_text(json.dumps(results, indent=2) + "\n")
        print(f"PASS {case['name']}: core/oracle replay, exact contents, repeat recovery, e2fsck", flush=True)


if __name__ == "__main__":
    main()
