#!/usr/bin/env python3
"""Select independently checked inline states for native Linux roundtrips."""

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
            raise RuntimeError(f"Inline Linux preparation command failed: {command}")
        return done.stdout

    for profile, state in (("no-checksum", "kept"), ("checksum-seed", "new"),
                           ("no-filetype", "directory-expanded"), ("inode512", "xattrs"),
                           ("ea-inode", "xattrs"), ("orphan-file", "new")):
        matches = [item for item in records if item.get("passed") and item["state"] == state
                   and Path(item["source"]).name == f"inline-{profile}.img"]
        if len(matches) != 1:
            raise RuntimeError(f"Missing unique checked inline state: {profile} {state}")
        checked = matches[0]
        source = Path(checked["image"])
        if digest(source) != checked["image_sha256"]:
            raise RuntimeError("Checked inline source changed")
        image = output / source.name
        shutil.copyfile(source, image)
        run([recover, "--write", image])
        actual = snapshot(image, output / f"{image.stem}-state", tools, run)
        selected.append(dict(image=str(image), input_sha256=digest(image), passed=True,
                             inline_data=True, block_size=actual["accounting"]["Block size"],
                             source=str(source), source_sha256=digest(source)))
        if digest(source) != checked["image_sha256"] or digest(recover) != recover_hash:
            raise RuntimeError("Inline preparation changed a protected input")
    (output / "selection.json").write_text(json.dumps(selected, indent=2) + "\n")
    print(f"Prepared {len(selected)} independently checked inline Linux inputs", flush=True)


if __name__ == "__main__":
    main()
