#!/usr/bin/env python3
"""Reproduce e2fsck attribute loss on a linked orphan without executing the core."""

import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess

from check_orphans import digest
from generate_fixtures import resolve_tools


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, required=True,
                        help="clean xattr fixture containing the empty linked /block inode")
    parser.add_argument("--tools-root", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    tools = resolve_tools(args.tools_root)
    source = args.image.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    original_sha = digest(source)
    record = dict(source=str(source), source_sha256=original_sha, commands=[],
                  ext4_core_executed=False, reproduced=False)

    def save():
        (output / "report.json").write_text(json.dumps(record, indent=2) + "\n")

    def require(condition, message):
        if not condition:
            save()
            raise RuntimeError(message)

    def run(command, allowed=(0,)):
        done = subprocess.run([str(part) for part in command], capture_output=True,
                              text=True, errors="backslashreplace", timeout=120)
        record["commands"].append(dict(command=[str(part) for part in command],
                                       status=done.returncode, stdout=done.stdout, stderr=done.stderr))
        require(done.returncode in allowed, f"Unexpected command result: {command}")
        save()
        return done.stdout

    def stat(image):
        raw = run([tools["debugfs"], "-R", "stat /block", image])
        patterns = dict(inode=r"Inode:\s+(\d+)", links=r"Links:\s+(\d+)",
                        size=r"Size:\s+(\d+)", blocks=r"Blockcount:\s+(\d+)",
                        attribute=r"^File ACL:\s+(\d+)")
        fields = {}
        for name, pattern in patterns.items():
            found = re.search(pattern, raw, re.M)
            require(found is not None, f"Missing independent inode field: {name}")
            fields[name] = int(found[1])
        return fields

    run([tools["e2fsck"], "-fn", source])
    before = stat(source)
    require(before["size"] == 0 and before["links"] == 1 and before["attribute"] != 0 and
            before["blocks"] > 0, "Expected an empty linked inode with an external attribute")
    pending = output / "linked-orphan.img"
    shutil.copyfile(source, pending)
    run([tools["debugfs"], "-w", "-R", f"set_super_value last_orphan {before['inode']}", pending])
    recovered = output / "e2fsck-recovered.img"
    shutil.copyfile(pending, recovered)
    run([tools["e2fsck"], "-y", "-E", "journal_only", recovered], allowed=(0, 1))
    after = stat(recovered)
    require(after["attribute"] == 0 and all(after[k] == before[k] for k in
            ("inode", "links", "size", "blocks")), "Expected attribute-loss defect was not reproduced")
    diagnostic = run([tools["e2fsck"], "-fn", recovered], allowed=(4,))
    require(f"Inode {before['inode']}, i_blocks is {before['blocks']}, should be 0" in diagnostic,
            "Expected stale attribute block accounting diagnostic")
    require(digest(source) == original_sha, "Protected source changed")
    record.update(reproduced=True, source_unchanged=True, before=before, after=after,
                  pending_sha256=digest(pending), recovered_sha256=digest(recovered),
                  oracle_recovery_passed=False)
    save()
    print("REPRODUCED: e2fsck loses a linked orphan external attribute; no ext4 core executed")


if __name__ == "__main__":
    main()
