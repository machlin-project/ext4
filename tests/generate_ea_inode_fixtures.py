#!/usr/bin/env python3
"""Create large attribute values with e2fsprogs and independently verify them."""

import argparse
import json
import os
from pathlib import Path
import re
import subprocess

from check_namespace import inode_fields
from generate_fixtures import EXPECTED_FEATURES, UUID, resolve_tools
from generate_xattr_fixtures import payload

PROFILES = (
    dict(name="ea-inode-1k"),
    dict(name="ea-inode-4k", block_size=4096),
    dict(name="ea-inode-no-checksum", exclude={"metadata_csum"}),
    dict(name="ea-inode-checksum-seed", include={"metadata_csum_seed"}),
    dict(name="ea-inode-inode128", inode_size=128, exclude={"extra_isize"}),
    dict(name="ea-inode-orphan-file", include={"orphan_file"}),
)
VALUE_MAX = 65536


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tools-root", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    tools = resolve_tools(args.tools_root)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    # debugfs ea_set -f reads at most one filesystem block. Use the independent
    # library's public API so the maximum-length fixture is actually 64 KiB.
    build = args.tools_root.resolve() if args.tools_root else tools["debugfs"].parent.parent
    helper = output / "ea-inode-fixture"
    command = [os.environ.get("CC", "cc"), "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
               "-Wdeclaration-after-statement", f"-I{build / 'lib'}", f"-I{build.parent / 'lib'}",
               str(Path(__file__).with_name("ea_inode_fixture.c")),
               str(build / "lib/libext2fs.a"), str(build / "lib/libcom_err.a"),
               "-lpthread", "-o", str(helper)]
    compiled = subprocess.run(command, capture_output=True, text=True)
    (output / "helper-build.json").write_text(json.dumps(dict(
        command=command, status=compiled.returncode, stdout=compiled.stdout, stderr=compiled.stderr), indent=2) + "\n")
    compiled.check_returncode()
    reports = []
    for profile in PROFILES:
        block_size = profile.get("block_size", 1024)
        inode_size = profile.get("inode_size", 256)
        name = profile["name"]
        image = output / f"{name}.img"
        tree = output / f"{name}-tree"
        tree.mkdir()
        for path in ("body", "many", "plain", "alias"):
            (tree / path).write_bytes(b"")
        record = dict(image=str(image), block_size=block_size, inode_size=inode_size,
                      commands=[], objects={}, cases=[])
        reports.append(record)

        def save():
            (output / "report.json").write_text(json.dumps(reports, indent=2) + "\n")

        def run(command):
            result = subprocess.run([str(x) for x in command], capture_output=True, timeout=120)
            record["commands"].append(dict(command=[str(x) for x in command],
                                           status=result.returncode,
                                           stdout=result.stdout.decode("utf-8", "backslashreplace"),
                                           stderr=result.stderr.decode("utf-8", "backslashreplace")))
            save()
            result.check_returncode()
            return result.stdout.decode("utf-8")

        def debug(command, write=False):
            return run([tools["debugfs"], *(["-w"] if write else []), "-R", command, image])

        features = ((EXPECTED_FEATURES - {"resize_inode"} - profile.get("exclude", set())) |
                    {"ea_inode"} | profile.get("include", set()))
        extended = "lazy_itable_init=0,lazy_journal_init=0,nodiscard"
        if "orphan_file" in features:
            extended += ",orphan_file_size=4"
        run([tools["mke2fs"], "-F", "-t", "ext4", "-b", block_size, "-g", 1024,
             "-N", 512, "-I", inode_size, "-m", 0, "-O", "none," + ",".join(sorted(features)),
             "-U", UUID, "-E", extended, "-d", tree, image, 16 * 1024 * 1024 // block_size])
        values = {"maximum": payload(VALUE_MAX), "medium": payload(3 * block_size + 7),
                  "small": payload(13), "empty": b""}
        for label, value in values.items():
            (output / f"{name}-{label}.data").write_bytes(value)
        cases = [("/body", "maximum", "maximum"), ("/body", "small", "small"),
                 ("/body", "empty", "empty")]
        cases += [("/many", f"value{index}", "medium") for index in range(8)]
        for path, key, label in cases:
            source = output / f"{name}-{label}.data"
            run([helper, image, path, f"user.{key}", source])
            extracted = output / f"{name}-{path[1:]}-{key}.value"
            debug(f'ea_get -r -f "{extracted}" {path} user.{key}')
            if extracted.read_bytes() != values[label]:
                raise RuntimeError("Independent large-attribute value mismatch")
            record["cases"].append(dict(path=path, key=key, value=str(source), size=len(values[label])))
        for path in ("/", "/body", "/many", "/plain", "/alias"):
            text = debug(f"stat {path}")
            record["objects"][path] = inode_fields(text)
            if path == "/many":
                match = re.search(r"^File ACL:\s+(\d+)", text, re.M)
                if not match or int(match[1]) == 0:
                    raise RuntimeError("Large-value names did not reach external attribute storage")
        header = run([tools["dumpe2fs"], "-h", image])
        if "ea_inode" not in re.search(r"^Filesystem features:\s+(.+)$", header, re.M)[1].split():
            raise RuntimeError("Missing EA_INODE feature")
        run([tools["e2fsck"], "-fn", image])
        manifest = "".join(f"{path} 1 {key.encode().hex()} {name}-{label}.data\n"
                           for path, key, label in cases)
        (output / f"{name}.xattrs").write_text(manifest)
        record["passed"] = True
        save()
        print(f"PASS {name}: {len(cases)} exact values, inode/body/block storage and clean fsck", flush=True)


if __name__ == "__main__":
    main()
