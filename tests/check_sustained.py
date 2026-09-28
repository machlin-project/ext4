#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Independently verify exported sustained-operation images with e2fsprogs."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

from check_namespace import inode_fields, symlink_bytes
from check_rename import entries
from generate_fixtures import resolve_tools

KIND_TYPES = {"file": "regular", "directory": "directory", "symlink": "symlink",
              "fifo": "FIFO", "device": "character"}
PERMISSION_BITS = 0o7777


def parse_manifest(path):
    root = None
    objects = []
    xattrs = {}
    for line in path.read_text().splitlines():
        fields = line.split(" ")
        if fields[0] == "root":
            root = fields[1]
        elif fields[0] == "xattr":
            number, key, value = int(fields[1]), fields[2], bytes.fromhex(fields[3])
            xattrs.setdefault(number, {})[key] = value
        else:
            kind, number, links, mode, name = fields
            if kind not in KIND_TYPES or not name.startswith("/"):
                raise RuntimeError(f"Malformed manifest line: {line!r}")
            objects.append(dict(kind=kind, number=int(number), links=int(links),
                                mode=int(mode, 8), path=name))
    if root is None:
        raise RuntimeError("Manifest lacks the sustained root")
    return root, objects, xattrs


def check_export(image, manifest, directory, output, tools, run):
    root, objects, xattrs = parse_manifest(manifest)
    block_size = None
    run([tools["e2fsck"], "-fn", image])
    info = run([tools["dumpe2fs"], "-h", image])
    match = re.search(r"^Block size:\s+(\d+)", info, re.M)
    if match is None:
        raise RuntimeError("Missing independently decoded block size")
    block_size = int(match[1])
    expected_children = {"": set()}
    paths = {}
    for item in objects:
        parent, _, name = item["path"].rpartition("/")
        expected_children.setdefault(parent, set()).add(name)
        if item["kind"] == "directory":
            expected_children.setdefault(item["path"], set())
        paths.setdefault(item["number"], item["path"])
    for parent, names in expected_children.items():
        listing = entries(run([tools["debugfs"], "-R", f'ls -p "/{root}{parent}"', image]))
        actual = set(listing) - {".", ".."}
        if actual != names:
            raise RuntimeError(f"Directory /{root}{parent} differs: "
                               f"missing {sorted(names - actual)[:4]}, "
                               f"extra {sorted(actual - names)[:4]}")
    dumps = 0
    for item in objects:
        path = f'"/{root}{item["path"]}"'
        inode = inode_fields(run([tools["debugfs"], "-R", f"stat {path}", image]))
        if inode is None or inode["inode"] != item["number"]:
            raise RuntimeError(f"Inode identity differs for {item['path']}")
        if inode["type"] != KIND_TYPES[item["kind"]]:
            raise RuntimeError(f"Inode type differs for {item['path']}")
        if inode["links"] != item["links"] or inode["mode"] & PERMISSION_BITS != item["mode"]:
            raise RuntimeError(f"Link count or permissions differ for {item['path']}")
        data = directory / f"object-{item['number']}.data"
        if item["kind"] == "file":
            dump = output / f"file-{dumps:05d}"
            dumps += 1
            run([tools["debugfs"], "-R", f'dump {path} "{dump}"', image])
            actual = hashlib.sha256(dump.read_bytes()).hexdigest()
            dump.unlink()
            if actual != hashlib.sha256(data.read_bytes()).hexdigest():
                raise RuntimeError(f"File contents differ for {item['path']}")
        elif item["kind"] == "symlink":
            target = symlink_bytes(image, path, inode, block_size, tools["debugfs"], run)
            if target != data.read_bytes():
                raise RuntimeError(f"Symlink target differs for {item['path']}")
    for number, values in xattrs.items():
        path = f'"/{root}{paths[number]}"'
        listed = dict(re.findall(r'^\s+([^\s]+)\s+\((\d+)\)',
                                 run([tools["debugfs"], "-R", f"ea_list {path}", image]), re.M))
        if set(listed) != set(values):
            raise RuntimeError(f"Attribute keys differ for {paths[number]}")
        for key, value in values.items():
            dump = output / f"attribute-{dumps:05d}"
            dumps += 1
            run([tools["debugfs"], "-R", f'ea_get -r -f "{dump}" {path} "{key}"', image])
            if dump.read_bytes() != value:
                raise RuntimeError(f"Attribute {key} differs for {paths[number]}")
            dump.unlink()
    return dict(objects=len(objects), directories=len(expected_children),
                xattrs=sum(len(values) for values in xattrs.values()))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--tools-root", type=Path, required=True)
    parser.add_argument("--exports", type=Path, required=True, action="append",
                        help="directory holding one exported image and its manifest")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    tools = resolve_tools(args.tools_root)
    args.output.mkdir(parents=True, exist_ok=False)
    rows = []

    for directory in args.exports:
        images = sorted(directory.glob("sustained-*.img"))
        if len(images) != 1:
            raise RuntimeError(f"Expected one exported image in {directory}")
        image = images[0]
        row = dict(export=str(directory), image=image.name, commands=[], passed=False)
        rows.append(row)
        before = hashlib.sha256(image.read_bytes()).hexdigest()

        def run(command, row=row):
            command = [str(part) for part in command]
            done = subprocess.run(command, capture_output=True, text=True, errors="replace",
                                  check=False)
            row["commands"].append(dict(command=command, status=done.returncode,
                                        stderr=done.stderr[-2000:]))
            if done.returncode != 0:
                raise RuntimeError(f"Command failed ({done.returncode}): {command}\n"
                                   f"{done.stdout[-2000:]}{done.stderr[-2000:]}")
            return done.stdout

        work = args.output / directory.name
        work.mkdir()
        row.update(check_export(image, directory / "manifest.txt", directory, work, tools, run))
        if hashlib.sha256(image.read_bytes()).hexdigest() != before:
            raise RuntimeError("Independent verification changed the exported image")
        row["passed"] = True
        (args.output / "report.json").write_text(json.dumps(rows, indent=2) + "\n")
        print(f"PASS sustained export {directory.name}: strict e2fsck, {row['objects']} names, "
              f"{row['xattrs']} attributes", flush=True)


if __name__ == "__main__":
    main()
