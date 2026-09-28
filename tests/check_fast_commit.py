#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Check core fast-commit replay with nonrepairing e2fsck and independent file reads."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess

from generate_fast_commit_fixtures import COMMITS, PROFILES, digest
from generate_fixtures import resolve_tools
from fast_commit_reference import read_namespace


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixtures", required=True, type=Path)
    parser.add_argument("--tools-root", required=True, type=Path)
    parser.add_argument("--recover", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    fixtures = args.fixtures.resolve()
    recover = args.recover.resolve()
    identity = digest(recover)
    tools = resolve_tools(args.tools_root.resolve())
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    source_rows = json.loads((fixtures / "report.json").read_text())
    if not source_rows or not all(row.get("passed") for row in source_rows):
        raise RuntimeError("Fixture generation is not accepted")
    cases = {row["profile"]: row for row in source_rows if "profile" in row}
    if set(cases) != {profile[0] for profile in PROFILES}:
        raise RuntimeError("Incomplete fast-commit fixture profile inventory")
    rows = []

    def run(row, command):
        command = [str(part) for part in command]
        done = subprocess.run(command, capture_output=True, text=True, timeout=300)
        row["commands"].append(dict(command=command, status=done.returncode,
                                    stdout=done.stdout, stderr=done.stderr))
        (output / "report.json").write_text(json.dumps(rows, indent=2) + "\n")
        if done.returncode:
            raise RuntimeError(f"Independent fast-commit check failed: {command}: {done.stderr}")
        return done.stdout

    for profile, _, _, _ in PROFILES:
        expected = cases[profile]
        source = fixtures / profile / "pending.img"
        if digest(source) != expected["pending_sha256"] or digest(recover) != identity:
            raise RuntimeError("Pending input or recovery executable changed")
        directory = output / profile
        directory.mkdir()
        image = directory / "recovered.img"
        shutil.copyfile(source, image)
        row = dict(profile=profile, commands=[], passed=False, recover_sha256=identity,
                   pending_sha256=expected["pending_sha256"])
        rows.append(row)
        text = run(row, [recover, "--write", image])
        if f"fast_commits={COMMITS}\n" not in text:
            raise RuntimeError("Core did not recover the expected fast-commit prefix")
        run(row, [tools["e2fsck"], "-fn", image])
        exported = directory / "files"
        exported.mkdir()
        actual = read_namespace(image, exported, expected["block_size"], tools["debugfs"],
                                lambda command: run(row, command))
        if any(actual[key] != expected[key]
               for key in ("files", "directories", "symlinks", "special", "xattrs")):
            raise RuntimeError(f"{profile}: independently read namespace or data differs")
        recovered_digest = digest(image)
        run(row, [recover, "--write", image])
        if digest(image) != recovered_digest or digest(source) != expected["pending_sha256"]:
            raise RuntimeError("Clean recovery changed the image or protected input changed")
        row.update(passed=True, recovered_sha256=recovered_digest, files=len(actual["files"]),
                   symlinks=len(actual["symlinks"]), special=len(actual["special"]),
                   xattrs=sum(len(values) for values in actual["xattrs"].values()),
                   clean_recovery_unchanged=True)
        (output / "report.json").write_text(json.dumps(rows, indent=2) + "\n")
        print(f"PASS fast commit {profile}: core replay, strict e2fsck, independent namespace/data",
              flush=True)


if __name__ == "__main__":
    main()
