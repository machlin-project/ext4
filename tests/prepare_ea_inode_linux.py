#!/usr/bin/env python3
"""Select checked EA_INODE states for the isolated native Linux roundtrip."""

import argparse
import json
from pathlib import Path
import shutil
import subprocess

from check_orphans import digest
from generate_fixtures import resolve_tools
from linux_xattrs import snapshot


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--report", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--recover", required=True, type=Path)
    parser.add_argument("--tools-root", type=Path)
    parser.add_argument("--profile", action="append", choices=(
        "4k", "no-checksum", "checksum-seed", "orphan-file"))
    args = parser.parse_args()
    records = json.loads(args.report.read_text())
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    recover = args.recover.resolve()
    recover_hash = digest(recover)
    tools = resolve_tools(args.tools_root)
    selected = []
    commands = []

    def run(command, allowed=(0,)):
        done = subprocess.run([str(x) for x in command], capture_output=True,
                              text=True, errors="backslashreplace", timeout=120)
        commands.append(dict(command=[str(x) for x in command], status=done.returncode,
                             stdout=done.stdout, stderr=done.stderr))
        (output / "commands.json").write_text(json.dumps(commands, indent=2) + "\n")
        if done.returncode not in allowed:
            raise RuntimeError(f"EA_INODE Linux preparation command failed: {command}")
        return done.stdout

    for profile, state in (("4k", "created-mapped"), ("no-checksum", "created-file"),
                           ("checksum-seed", "created-directory"), ("orphan-file", "created")):
        if args.profile and profile not in args.profile:
            continue
        matches = [item for item in records if item.get("passed") and item["state"] == state
                   and Path(item["source"]).name == f"ea-inode-{profile}.img"]
        if len(matches) != 1:
            raise RuntimeError(f"Missing unique checked EA_INODE state: {profile} {state}")
        checked = matches[0]
        source = Path(checked["exported"])
        if digest(source) != checked["exported_sha256"]:
            raise RuntimeError("Checked EA_INODE source changed")
        image = output / source.name
        shutil.copyfile(source, image)
        run([recover, "--write", image])
        actual = snapshot(image, output / f"{image.stem}-state", tools, run)
        selected.append(dict(image=str(image), input_sha256=digest(image), passed=True,
                             ea_inode=True, block_size=actual["accounting"]["Block size"],
                             source=str(source), source_sha256=digest(source)))
        if digest(source) != checked["exported_sha256"] or digest(recover) != recover_hash:
            raise RuntimeError("EA_INODE preparation changed a protected input")
    (output / "selection.json").write_text(json.dumps(selected, indent=2) + "\n")
    print(f"Prepared {len(selected)} independently checked EA_INODE Linux inputs", flush=True)


if __name__ == "__main__":
    main()
