#!/usr/bin/env python3
"""Create clean filesystems without EXT_ATTR for first-attribute transaction tests."""

import argparse
import json
from pathlib import Path
import re
import subprocess

from check_namespace import inode_fields
from check_orphans import accounting, digest
from generate_fixtures import EXPECTED_FEATURES, UUID, resolve_tools
from generate_xattr_fixtures import XATTR_PROFILES


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tools-root", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    tools = resolve_tools(args.tools_root)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    records = []

    def save():
        (output / "report.json").write_text(json.dumps(records, indent=2) + "\n")

    for profile in XATTR_PROFILES:
        name = profile["name"].replace("xattr-", "xattr-enable-", 1)
        block_size = profile.get("block_size", 1024)
        inode_size = profile.get("inode_size", 256)
        features = (EXPECTED_FEATURES - {"resize_inode", "ext_attr"} -
                    profile.get("exclude", set()) | profile.get("include", set()))
        image_bytes = max(8 * 1024 * 1024, block_size * 2048)
        tree = output / (name + "-tree")
        tree.mkdir()
        (tree / "plain").write_bytes(b"")
        (tree / "directory").mkdir()
        image = output / (name + ".img")
        record = dict(image=str(image), block_size=block_size, inode_size=inode_size, commands=[])
        records.append(record)

        def require(condition, message):
            if not condition:
                save()
                raise RuntimeError(f"{name}: {message}")

        def run(command):
            done = subprocess.run([str(part) for part in command], capture_output=True,
                                  text=True, errors="backslashreplace", timeout=120)
            record["commands"].append(dict(command=[str(part) for part in command],
                                           status=done.returncode, stdout=done.stdout, stderr=done.stderr))
            require(done.returncode == 0, f"Independent command failed: {command}")
            return done.stdout

        extended = "lazy_itable_init=0,lazy_journal_init=0,nodiscard"
        if "orphan_file" in features:
            extended += ",orphan_file_size=4"
        run([tools["mke2fs"], "-F", "-t", "ext4", "-b", block_size, "-g", 1024,
             "-N", 256, "-I", inode_size, "-m", 0, "-O", "none," + ",".join(sorted(features)),
             "-U", UUID, "-E", extended, "-d", tree, image, image_bytes // block_size])
        run([tools["e2fsck"], "-fn", image])
        header = run([tools["dumpe2fs"], "-h", image])
        found = re.search(r"^Filesystem features:\s+(.+)$", header, re.M)
        require(found is not None and set(found[1].split()) == features,
                "Feature profile differs; EXT_ATTR must be absent")
        objects = {}
        for path in ("/", "/lost+found", "/plain", "/directory"):
            stat = run([tools["debugfs"], "-R", f"stat {path}", image])
            objects[path] = inode_fields(stat)
            require(objects[path] is not None, f"Missing fixture inode {path}")
            require(re.search(r"^File ACL:\s+0$", stat, re.M), f"External attributes on {path}")
            listing = run([tools["debugfs"], "-R", f"ea_list {path}", image])
            require(not re.search(r"^  .+ \(\d+\)", listing, re.M), f"Attributes on {path}")
        record.update(passed=True, input_sha256=digest(image), features=sorted(features),
                      objects=objects, accounting=accounting(header))
        save()
        print(f"PASS {name}: clean image without EXT_ATTR or inode attributes", flush=True)


if __name__ == "__main__":
    main()
