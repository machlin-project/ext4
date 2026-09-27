#!/usr/bin/env python3
"""Verify exact retained names and accounting after reuse of a full-root index."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess

from check_index_write import entries, MUTATION_TIME
from check_namespace import inode_fields
from check_orphans import accounting, digest
from generate_fixtures import resolve_tools


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture", type=Path, required=True, help="Capacity fixture report.json")
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--tools-root", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--case", help="Select one exact source image filename from a fixture package")
    args = parser.parse_args()
    tools = resolve_tools(args.tools_root)
    fixtures = json.loads(args.fixture.read_text())
    if args.case:
        fixtures = [fixture for fixture in fixtures if Path(fixture["image"]).name == args.case]
    if len(fixtures) != 1 or not fixtures[0].get("passed"):
        raise RuntimeError("Expected one verified full-root fixture")
    fixture = fixtures[0]
    source = Path(fixture["image"])
    image = args.image.resolve()
    image_hash = digest(image)
    if digest(source) != fixture["input_sha256"]:
        raise RuntimeError("Capacity source fixture changed")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    report = dict(source=str(source), source_sha256=fixture["input_sha256"], image=str(image),
                  input_sha256=image_hash, commands=[], objects={})

    def save():
        (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")

    def require(condition, message):
        if not condition:
            save()
            raise RuntimeError(message)

    def run(command, raw=False):
        done = subprocess.run([str(part) for part in command], capture_output=True, timeout=120)
        report["commands"].append(dict(command=[str(part) for part in command], status=done.returncode,
                                        stdout_sha256=hashlib.sha256(done.stdout).hexdigest(), stdout_bytes=len(done.stdout),
                                        stderr=done.stderr.decode("utf-8", "backslashreplace")))
        require(done.returncode == 0, "Capacity oracle command failed")
        return done.stdout if raw else done.stdout.decode("utf-8", "backslashreplace")

    run([tools["e2fsck"], "-fn", image])
    before_counts = accounting(run([tools["dumpe2fs"], "-h", source]))
    after_counts = accounting(run([tools["dumpe2fs"], "-h", image]))
    require(before_counts == after_counts, "Full-root record reuse changed allocation accounting")
    paths = ["/", "/indexed", "/indexed/child", "/lost+found", "/hello.txt", "/alternate.txt"]
    if "/peer" in fixture.get("lookup", {}).get("directories", {}):
        paths += ["/peer", "/peer/child"]
    for path in paths:
        before = inode_fields(run([tools["debugfs"], "-R", f"stat {path}", source]))
        after = inode_fields(run([tools["debugfs"], "-R", f"stat {path}", image]))
        require(before is not None and after is not None, f"Missing capacity object {path}")
        expected = before
        if path == "/indexed":
            expected = dict(before, mtime=MUTATION_TIME, ctime=MUTATION_TIME)
        elif path == "/hello.txt":
            expected = dict(before, links=before["links"] + 1, ctime=MUTATION_TIME)
        require(after == expected, f"Full-root reuse changed unrelated attributes {path}")
        if before["type"] == "directory":
            old_names = entries(run([tools["debugfs"], "-R", f"ls -p {path}", source], raw=True))
            new_names = entries(run([tools["debugfs"], "-R", f"ls -p {path}", image], raw=True))
            if path == "/indexed":
                require(len(old_names) == (fixture["entries"] if "entries" in fixture else
                                          fixture["entries_per_directory"]),
                        "Capacity baseline lost expected names")
                target = inode_fields(run([tools["debugfs"], "-R", "stat /hello.txt", source]))
                old_names[b"capacity-reuse"] = target["inode"]
                report["retained_names"] = len(old_names)
            require(old_names == new_names, f"Full-root reuse lost, changed or duplicated a name in {path}")
        else:
            dumps = []
            for label, candidate in (("before", source), ("after", image)):
                dump = output / f"{path.strip('/')}-{label}.data"
                run([tools["debugfs"], "-R", f"dump {path} {dump}", candidate])
                dumps.append(dump.read_bytes())
            require(dumps[0] == dumps[1], f"Full-root reuse changed retained data {path}")
        report["objects"][path] = after
    require(digest(source) == fixture["input_sha256"] and digest(image) == image_hash, "Capacity checker changed input")
    report.update(passed=True, accounting=after_counts, protected_inputs_unchanged=True)
    save()
    print(f"PASS full-root record reuse: {report['retained_names']} exact names, unchanged allocation, attributes/data and fsck")


if __name__ == "__main__":
    main()
