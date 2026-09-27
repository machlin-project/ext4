#!/usr/bin/env python3
"""Independently inspect colliding names, empty continuation leaves and compaction."""

import argparse
import json
from pathlib import Path
import re
import subprocess

from check_index_write import entries, INODE_INDEX, MUTATION_TIME
from check_namespace import inode_fields
from check_orphans import accounting, digest
from generate_fixtures import resolve_tools


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixtures", type=Path, required=True)
    parser.add_argument("--vectors", type=Path, required=True, help="Verified collision report.json")
    parser.add_argument("--exports", type=Path, required=True)
    parser.add_argument("--tools-root", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--case", action="append", default=[], help="Select an exact 1 KiB source image filename")
    args = parser.parse_args()
    tools = resolve_tools(args.tools_root)
    fixtures = json.loads(args.fixtures.read_text())
    vectors = json.loads(args.vectors.read_text())
    if not fixtures or not all(item.get("passed") for item in fixtures) or not vectors.get("passed"):
        raise RuntimeError("Expected independently checked fixtures and collisions")
    if args.case:
        if set(args.case) - {Path(item["image"]).name for item in fixtures if item["block_size"] == 1024}:
            raise RuntimeError("Requested 1 KiB indexed edge profile is absent")
        fixtures = [item for item in fixtures if Path(item["image"]).name in args.case]
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    records = []

    def save():
        (output / "report.json").write_text(json.dumps(records, indent=2) + "\n")

    for fixture in fixtures:
        if fixture["block_size"] != 1024:
            continue
        version = fixture["hash_version"] + (3 if fixture["signedness"] == "unsigned" else 0)
        vector = next(item for item in vectors["records"] if item["version"] == version and item["passed"])
        first, second = (bytes.fromhex(item["name_hex"]) for item in vector["names"])
        source = Path(fixture["image"])
        if digest(source) != fixture["input_sha256"]:
            raise RuntimeError("Indexed edge source changed")
        baseline = None
        for kind in ("collided", "collision-retained", "compacted"):
            image = args.exports.resolve() / f"indexed-{kind}-{source.name}"
            image_hash = digest(image)
            record = dict(image=str(image), input_sha256=image_hash, source=str(source),
                          source_sha256=fixture["input_sha256"], kind=kind, version=version, commands=[])
            records.append(record)

            def require(condition, message):
                if not condition:
                    save()
                    raise RuntimeError(message)

            def run(command, raw=False):
                parts = [bytes(part) if isinstance(part, Path) else part if isinstance(part, bytes)
                         else str(part).encode() for part in command]
                done = subprocess.run(parts, capture_output=True, timeout=120)
                record["commands"].append(dict(command=[part.decode("utf-8", "backslashreplace") for part in parts],
                                                status=done.returncode,
                                                stdout=done.stdout.decode("utf-8", "backslashreplace"),
                                                stderr=done.stderr.decode("utf-8", "backslashreplace")))
                require(done.returncode == 0, "Independent edge command failed")
                return done.stdout if raw else done.stdout.decode("utf-8", "backslashreplace")

            def stat(candidate, path):
                inode = inode_fields(run([tools["debugfs"], "-R", f"stat {path}", candidate]))
                require(inode is not None, f"Missing edge object {path}")
                return inode

            def names(candidate, path):
                return entries(run([tools["debugfs"], "-R", f"ls -p {path}", candidate], raw=True))

            def oracle_hash(name):
                command = f"dx_hash -h {version} -s {vectors['seed']} ".encode() + b'"' + name + b'"'
                payload = run([tools["debugfs"], "-R", command], raw=True)
                found = re.search(rb" is 0x([0-9a-f]+) \(minor 0x([0-9a-f]+)\)\n$", payload)
                require(found is not None, "Missing independent name hash")
                return int(found[1], 16)

            run([tools["e2fsck"], "-fn", image])
            before = accounting(run([tools["dumpe2fs"], "-h", source]))
            after = accounting(run([tools["dumpe2fs"], "-h", image]))
            old_parent = stat(source, "/indexed")
            parent = stat(image, "/indexed")
            old_child = stat(source, "/indexed/child")
            old_hello = stat(source, "/hello.txt")
            hello = stat(image, "/hello.txt")
            listing = names(image, "/indexed")
            for path in ("/", "/peer", "/peer/child", "/lost+found"):
                require(stat(source, path) == stat(image, path) and names(source, path) == names(image, path),
                        f"Indexed edge changed unrelated directory {path}")
            require(listing.pop(b".") == parent["inode"] and
                    listing.pop(b"..") == names(image, "/")[b"."], "Edge dot records changed")
            expected_parent = dict(old_parent, links=2, size=parent["size"], blocks=parent["blocks"],
                                   mtime=MUTATION_TIME, ctime=MUTATION_TIME)
            require(parent == expected_parent and parent["flags"] & INODE_INDEX,
                    "Indexed edge changed unrelated parent metadata")
            htree = run([tools["debugfs"], "-R", "htree_dump /indexed", image], raw=True)
            other = None
            if kind == "collided":
                require(len(listing) == 4 and listing.get(first) == hello["inode"] and
                        second in listing and listing[second] != hello["inode"], "Collision names lost distinct identities")
                guards = set(listing) - {first, second}
                require(len(guards) == 2 and all(re.fullmatch(rb"edge-[0-9a-f]{8}-g{241}", name) for name in guards),
                        "Unexpected collision guard names")
                low = [name for name in guards if oracle_hash(name) < vector["major"]]
                high = [name for name in guards if oracle_hash(name) > vector["major"]]
                require(len(low) == len(high) == 1 and all(listing[name] == hello["inode"] for name in guards),
                        "Collision guards do not straddle the independently confirmed hash")
                leaf_names = {}
                for match in re.finditer(rb"Reading directory block (\d+),[^\n]*\n(.*?)(?=Reading directory block |\Z)",
                                         htree, re.S):
                    for name in (first, second):
                        if name in match[2]:
                            require(name not in leaf_names, "Collision name appears in multiple leaves")
                            leaf_names[name] = int(match[1])
                require(len(leaf_names) == 2 and leaf_names[first] != leaf_names[second],
                        "Independent index dump did not show a collision across leaves")
                baseline = dict(names=listing, low=low[0], high=high[0], leaves=leaf_names)
            elif kind == "collision-retained":
                require(listing == {second: baseline["names"][second], baseline["high"]: hello["inode"]},
                        "Collision deletion lost the continuation or retained a removed name")
                empty = re.search(rb"Reading directory block " + str(baseline["leaves"][first]).encode() +
                                  rb",[^\n]*\n(.*?)(?=Reading directory block |\Z)", htree, re.S)
                require(empty is not None and not any(int(number) for number in
                        re.findall(rb"(?:^|\n)\s*(\d+) 0x[0-9a-f]+-", empty[1])),
                        "Expected empty leading leaf of the collision chain")
            else:
                require(len(listing) == 6 and listing.get(first) == hello["inode"] and
                        all(number == hello["inode"] for number in listing.values()), "Compaction lost hardlink names")
                short = set(listing) - {first}
                require(len(short) == 5 and all(re.fullmatch(rb"edge-[0-9a-f]{8}-g{82}", name) for name in short),
                        "Compaction changed retained short name bytes")
                require(parent["size"] == old_parent["size"] and parent["blocks"] == old_parent["blocks"],
                        "Compaction allocated new directory space")
            if kind != "compacted":
                require(f"Hash 0x{vector['major'] | 1:08x}".encode() in htree,
                        "Missing odd collision continuation separator")
                other = stat(image, f"<{listing[second]}>")
                require((other["type"], other["mode"], other["uid"], other["gid"], other["links"],
                         other["size"], other["blocks"]) == ("regular", 0o750, 70000, 80000, 1, 0, 0) and
                        all(other[field] == MUTATION_TIME for field in ("atime", "mtime", "ctime")),
                        "Colliding newly created inode has incorrect metadata")
            hello_links = fixture["entries_per_directory"] - 3 + 1 + sum(number == hello["inode"] for number in listing.values())
            require(hello == dict(old_hello, links=hello_links, ctime=MUTATION_TIME), "Edge hardlink accounting changed incorrectly")
            require(after["Free inodes"] == before["Free inodes"] + int(kind == "compacted"),
                    "Edge inode reclamation/allocation count mismatch")
            require(after["Free blocks"] == before["Free blocks"] +
                    (old_child["blocks"] - (parent["blocks"] - old_parent["blocks"])) // (fixture["block_size"] // 512),
                    "Edge block accounting disagrees with released child and grown parent")
            dump = output / f"{image.stem}-hello.data"
            run([tools["debugfs"], "-R", f"dump /hello.txt {dump}", image])
            require(dump.read_bytes() == b"Machlin ext4\n", "Edge changed retained file data")
            require(digest(image) == image_hash and digest(source) == fixture["input_sha256"], "Edge checker changed protected input")
            record.update(passed=True, accounting=after, parent=parent, hello=hello, other=other,
                          names={name.hex(): number for name, number in listing.items()}, protected_inputs_unchanged=True)
            save()
            print(f"PASS {image.name}: exact collision/compaction objects, index, accounting and fsck", flush=True)


if __name__ == "__main__":
    main()
