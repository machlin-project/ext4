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
    parser.add_argument("--fixtures", required=True, type=Path)
    parser.add_argument("--recover", required=True, type=Path)
    parser.add_argument("--e2fsck", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    if output.exists():
        parser.error("output must be a new directory")
    output.mkdir(parents=True)
    fixtures = args.fixtures.resolve()
    manifest = json.loads((fixtures / "manifest.json").read_text())
    results = []
    for case in manifest["cases"]:
        source = fixtures / case["image"]
        if digest(source) != case["image_sha256"]:
            raise RuntimeError(f"fixture changed: {source}")
        image = output / source.name
        shutil.copyfile(source, image)
        result = {"name": case["name"], "commands": []}
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
        with image.open("rb") as file:
            for block, expected in zip(case["targets"], case["expected"], strict=True):
                file.seek(block * manifest["block_size"])
                if file.read(manifest["block_size"]) != (fixtures / expected).read_bytes():
                    raise RuntimeError(f"incorrect recovered bytes: {case['name']}, block {block}")
        before = digest(image)
        run([args.recover.resolve(), "--write", image])
        if digest(image) != before:
            raise RuntimeError("repeated recovery changed an already clean image")
        run([args.e2fsck.resolve(), "-fn", image])
        result["recovered_sha256"] = before
        result["passed"] = True
        (output / "report.json").write_text(json.dumps(results, indent=2) + "\n")
        print(f"PASS {case['name']}: exact contents, repeat recovery, e2fsck", flush=True)


if __name__ == "__main__":
    main()
