#!/usr/bin/env python3
"""Independently inspect namespace behavior with completely allocated block bitmaps."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess

from check_index_write import entries
from check_namespace import inode_fields, INODE_INDEX
from check_orphans import accounting, digest
from generate_fixtures import resolve_tools
from generate_space_fixtures import NAME_MAX, RESERVE_BLOCKS
from linux_namespace import symlink_bytes

MUTATION_TIME = (1700000060, 0)
PATHS = ("/", "/lost+found", "/linear", "/indexed", "/room", "/target", "/reserve", "/empty", "/filler")


def filename(index):
    return f"capacity-{index:06d}-".ljust(NAME_MAX, "n").encode()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixtures", type=Path, required=True)
    parser.add_argument("--exports", type=Path, required=True)
    parser.add_argument("--tools-root", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    tools = resolve_tools(args.tools_root)
    fixtures = json.loads(args.fixtures.read_text())
    if not fixtures or not all(item.get("passed") for item in fixtures):
        raise RuntimeError("Expected independently verified full-space fixtures")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    records = []

    def save():
        (output / "report.json").write_text(json.dumps(records, indent=2) + "\n")

    for fixture in fixtures:
        source = Path(fixture["image"])
        images = {kind: args.exports.resolve() / f"space-{kind}-{source.name}"
                  for kind in ("prepared", "reused", "linear-created", "indexed-created")}
        protected = {path: digest(path) for path in (source, *images.values())}
        record = dict(source=str(source), source_sha256=protected[source], commands=[], states={})
        records.append(record)
        save()

        def require(condition, message):
            if not condition:
                save()
                raise RuntimeError(f"{source.name}: {message}")

        def run(command, raw=False):
            done = subprocess.run([str(part) for part in command], capture_output=True, timeout=120)
            event = dict(command=[str(part) for part in command], status=done.returncode,
                         stdout_sha256=hashlib.sha256(done.stdout).hexdigest(), stdout_bytes=len(done.stdout),
                         stderr=done.stderr.decode("utf-8", "backslashreplace"))
            record["commands"].append(event)
            with (output / "commands.log").open("a") as stream:
                stream.write(json.dumps(dict(**event, stdout=done.stdout.decode("utf-8", "backslashreplace"))) + "\n")
            require(done.returncode == 0, f"Independent command failed: {command}")
            return done.stdout if raw else done.stdout.decode("utf-8", "backslashreplace")

        def stat(image, path):
            inode = inode_fields(run([tools["debugfs"], "-R", f"stat {path}", image]))
            require(inode is not None, f"Missing object {path} in {image.name}")
            return inode

        def listing(image, path):
            return entries(run([tools["debugfs"], "-R", f"ls -p {path}", image], raw=True))

        def data(image, path):
            label = hashlib.sha256(path.encode()).hexdigest()[:16]
            dump = output / f"{image.stem}-{label}.data"
            run([tools["debugfs"], "-R", f"dump {path} {dump}", image])
            return dump.read_bytes()

        def snapshot(image):
            run([tools["e2fsck"], "-fn", image])
            counts = accounting(run([tools["dumpe2fs"], "-h", image]))
            require(counts["Free blocks"] == 0, "Expected physically full filesystem")
            objects = {}
            for path in PATHS:
                inode = stat(image, path)
                mapping = run([tools["debugfs"], "-R", f"blocks {path}", image])
                require(all(value.isdigit() for value in mapping.split()), "Invalid independent mapping")
                item = dict(inode=inode, mapping=[int(value) for value in mapping.split()])
                contents = data(image, path)
                require(len(contents) == inode["size"], f"Data size differs from inode for {path}")
                item["data_sha256"] = hashlib.sha256(contents).hexdigest()
                if inode["type"] == "directory":
                    item["names"] = {name.hex(): number for name, number in listing(image, path).items()}
                objects[path] = item
            return dict(accounting=counts, objects=objects)

        def unchanged_except(before, after, changed):
            for path in PATHS:
                if path not in changed:
                    require(before["objects"][path] == after["objects"][path], f"Changed unrelated object {path}")

        def new_inode(image, path, kind, links, size, blocks):
            inode = stat(image, path)
            require((inode["type"], inode["uid"], inode["gid"], inode["mode"], inode["links"],
                     inode["size"], inode["blocks"]) == (kind, 70000, 80000, 0o750, links, size, blocks),
                    f"Incorrect created object {path}")
            require(inode["generation"] != 0 and all(inode[field] == MUTATION_TIME for field in ("atime", "mtime", "ctime")),
                    f"Created identity or captured times lost for {path}")
            require(inode["inode"] not in {item["inode"]["inode"] for item in old["objects"].values()},
                    "Creation reused an allocated inode")
            return inode

        require(protected[source] == fixture["input_sha256"], "Fixture changed before inspection")
        old = snapshot(source)
        prepared = snapshot(images["prepared"])
        require(old["accounting"] == prepared["accounting"], "Filling existing index slack changed accounting")
        old_parent = old["objects"]["/indexed"]
        parent = prepared["objects"]["/indexed"]
        prepared_count = len(parent["names"]) - len(old_parent["names"])
        target = old["objects"]["/target"]["inode"]
        expected_names = {**old_parent["names"], **{filename(index).hex(): target["inode"] for index in range(prepared_count)}}
        require(prepared_count >= 0 and parent["names"] == expected_names, "Preparation changed unexpected indexed names")
        expected = dict(old_parent["inode"])
        expected_target = dict(target)
        if prepared_count:
            expected.update(mtime=MUTATION_TIME, ctime=MUTATION_TIME)
            expected_target.update(links=target["links"] + prepared_count, ctime=MUTATION_TIME)
        require(parent["inode"] == expected and parent["mapping"] == old_parent["mapping"], "Preparation changed index allocation or identity")
        require(prepared["objects"]["/target"] == {**old["objects"]["/target"], "inode": expected_target},
                "Preparation lost target data, links or attributes")
        unchanged_except(old, prepared, {"/indexed", "/target"})
        record["states"]["prepared"] = dict(passed=True, added_index_names=prepared_count,
                                              image=str(images["prepared"]), input_sha256=protected[images["prepared"]],
                                              verified=prepared)
        block_size = fixture["block_size"]
        image = images["reused"]
        reused = snapshot(image)
        unchanged_except(prepared, reused, {"/room", "/target", "/empty"})
        created = new_inode(image, "/room/renamed", "regular", 1, 0, 0)
        link = new_inode(image, "/room/short-link", "symlink", 1, 9, 0)
        require(created["inode"] != link["inode"], "Distinct created objects alias one inode")
        require(symlink_bytes(image, "/room/short-link", link, block_size, tools["debugfs"], run) == b"../target",
                "Inline symlink target differs")
        room_old = prepared["objects"]["/room"]
        room = reused["objects"]["/room"]
        target_old = prepared["objects"]["/target"]["inode"]
        require(room["inode"] == dict(room_old["inode"], mtime=MUTATION_TIME, ctime=MUTATION_TIME) and
                room["mapping"] == room_old["mapping"] and room["names"] ==
                {**room_old["names"], b"renamed".hex(): created["inode"], b"short-link".hex(): link["inode"],
                 b"hardlink".hex(): target_old["inode"]}, "Record reuse changed unexpected room metadata or names")
        require(reused["objects"]["/target"]["inode"] ==
                dict(target_old, links=target_old["links"] + 1, mode=0o750, mtime=MUTATION_TIME, ctime=MUTATION_TIME),
                "Full-disk overwrite/hardlink changed incorrect target attributes")
        require(data(image, "/target") == b"T" * 7 + b"R" + b"T" * (block_size - 8) and
                reused["objects"]["/target"]["mapping"] == prepared["objects"]["/target"]["mapping"],
                "Full-disk overwrite changed incorrect data or mapping")
        empty_old = prepared["objects"]["/empty"]
        require(reused["objects"]["/empty"] ==
                {**empty_old, "inode": dict(empty_old["inode"], mode=0o750, mtime=MUTATION_TIME, ctime=MUTATION_TIME)},
                "Sparse growth/shrink changed empty file identity or allocation")
        require(reused["accounting"] == {**prepared["accounting"], "Free inodes": prepared["accounting"]["Free inodes"] - 2},
                "Record reuse has incorrect allocation accounting")
        record["states"]["reused"] = dict(passed=True, image=str(image), input_sha256=protected[image], verified=reused)

        for kind in ("linear", "indexed"):
            image = images[f"{kind}-created"]
            written = snapshot(image)
            path = f"/{kind}"
            unchanged_except(prepared, written, {path, "/reserve"})
            parent_old = prepared["objects"][path]
            parent = written["objects"][path]
            name = filename(prepared_count if kind == "indexed" else 0)
            child_path = path + "/" + name.decode()
            child = new_inode(image, child_path, "directory", 2, block_size, block_size // 512)
            require(listing(image, child_path) == {b".": child["inode"], b"..": parent["inode"]["inode"]},
                    "Full-space mkdir has incorrect dot entries")
            require(parent["names"] == {**parent_old["names"], name.hex(): child["inode"]},
                    "Full-space mkdir changed unexpected names")
            expected_parent = dict(parent_old["inode"], links=parent_old["inode"]["links"] + 1,
                                   mtime=MUTATION_TIME, ctime=MUTATION_TIME,
                                   size=parent["inode"]["size"], blocks=parent["inode"]["blocks"])
            if (kind == "linear" and parent_old["inode"]["size"] == block_size and
                    not parent_old["inode"]["flags"] & INODE_INDEX and
                    parent["inode"]["flags"] == parent_old["inode"]["flags"] | INODE_INDEX):
                require(parent["inode"]["size"] in (2 * block_size, 3 * block_size),
                        "Automatic index creation has an invalid size")
                expected_parent["flags"] |= INODE_INDEX
            require(parent["inode"] == expected_parent and
                    parent["inode"]["size"] > parent_old["inode"]["size"], "Mkdir lost parent metadata or did not grow")
            reserve_old = prepared["objects"]["/reserve"]["inode"]
            reserve = written["objects"]["/reserve"]["inode"]
            released = RESERVE_BLOCKS - reserve["size"] // block_size
            require(2 <= released <= RESERVE_BLOCKS and reserve["size"] % block_size == 0 and
                    reserve["blocks"] == reserve["size"] // 512 and
                    reserve == dict(reserve_old, size=reserve["size"], blocks=reserve["blocks"], mode=0o750,
                                    mtime=MUTATION_TIME, ctime=MUTATION_TIME) and
                    data(image, "/reserve") == b"S" * reserve["size"], "Released reserve lost bytes or attributes")
            require(parent["inode"]["blocks"] - parent_old["inode"]["blocks"] + child["blocks"] ==
                    released * block_size // 512, "Mkdir failed to account every released block")
            require(written["accounting"] == {**prepared["accounting"], "Free inodes": prepared["accounting"]["Free inodes"] - 1},
                    "Mkdir has incorrect allocation accounting")
            record["states"][f"{kind}-created"] = dict(passed=True, released_blocks=released,
                                                          image=str(image), input_sha256=protected[image],
                                                          created_path=child_path, verified=written)
        require(all(digest(path) == value for path, value in protected.items()), "Independent inspection changed an input")
        record.update(passed=True, protected_inputs_unchanged=True)
        save()
        print(f"PASS {source.name}: full bitmaps, record reuse and both mkdir capacity boundaries; exact data/names/attributes and fsck", flush=True)

    linux_cases = []
    for record in records:
        for state, item in record["states"].items():
            case = dict(passed=True, image=item["image"], input_sha256=item["input_sha256"],
                        space_state=state, verified_space=item["verified"], accounting=item["verified"]["accounting"])
            if "created_path" in item:
                case["created_path"] = item["created_path"]
            linux_cases.append(case)
    (output / "linux-inputs.json").write_text(json.dumps(linux_cases, indent=2) + "\n")


if __name__ == "__main__":
    main()
