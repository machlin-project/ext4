#!/usr/bin/env python3
"""Check transactional xattr exports and replay against independent e2fsprogs."""

import argparse
import copy
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

from check_namespace import inode_fields, symlink_bytes
from check_orphans import accounting, digest
from check_xattrs import PREFIXES, listed_name
from generate_fixtures import resolve_tools
from generate_xattr_fixtures import XATTR_BLOCK_HEADER, XATTR_HEADER_FIELDS, XATTR_MAGIC

OPERATIONS = ("create", "cow", "detach", "release", "replace", "metadata")
PATHS = dict(zip(OPERATIONS, ("/plain", "/block", "/block", "/symlink", "/mapped-symlink", "/many")))
CHANGE_SECONDS = 1700000070
SECTOR_BYTES = 512
INODE_BASE_BYTES = 128
XATTR_ENTRY_BYTES = 16
XATTR_ALIGNMENT = 4
XATTR_REGION_HEADER_BYTES = 4
XATTR_TERMINATOR_BYTES = 4
PACKING_GAP_BYTES = 36


def canonical(value):
    return json.loads(json.dumps(value))


def changed_value(size):
    return bytes((index * 17 + 0x51) & 255 for index in range(size))


def expected_state(fixture, values, counts, operation):
    objects = copy.deepcopy(fixture["objects"])
    values = dict(values)
    counts = dict(counts)
    sectors = fixture["block_size"] // SECTOR_BYTES
    if operation == "full":
        paths = ("/block", "/shared", "/capacity", "/symlink", "/mapped-symlink")
        if fixture["inode_size"] != INODE_BASE_BYTES:
            paths += ("/plain",)
    else:
        paths = {"references": ("/block", "/shared"), "capacity": ("/capacity",),
                 "packed": ("/plain",)}.get(operation, (PATHS.get(operation),))
    for path in paths:
        objects[path]["ctime"] = [CHANGE_SECONDS, 0]
    if operation == "create":
        values["/plain", 1, "tiny"] = b"abc"
        values["/plain", 1, "empty"] = b""
        values["/plain", 1, "high-é"] = changed_value(13)
        if fixture["inode_size"] == 128:
            objects["/plain"]["blocks"] += sectors
            counts["Free blocks"] -= 1
    elif operation in ("cow", "replace"):
        values[PATHS[operation], 1, "binary"] = changed_value(700)
        if operation == "cow":
            counts["Free blocks"] -= 1
    elif operation in ("detach", "release", "references"):
        for path in paths:
            del values[path, 1, "binary"]
            objects[path]["blocks"] -= sectors
        counts["Free blocks"] += operation != "detach"
    elif operation == "metadata":
        del values["/many", 1, "empty"]
        values["/many", 1, "binary"] = changed_value(700)
        values["/many", 1, "high-é"] = changed_value(13)
        values["/many", 6, "test"] = changed_value(19)
        objects["/many"].update(uid=70000, gid=80000, mode=0o761)
    elif operation == "capacity":
        values["/capacity", 1, "full"] = changed_value(fixture["largest_value"])
        if fixture["inode_size"] != INODE_BASE_BYTES:
            values["/capacity", 1, "tiny"] = b"abc"
    elif operation == "packed":
        body = fixture["inode_size"] - INODE_BASE_BYTES - fixture["plain_extra_size"] - XATTR_REGION_HEADER_BYTES - XATTR_TERMINATOR_BYTES
        capacity = fixture["block_size"] - XATTR_BLOCK_HEADER.size - XATTR_TERMINATOR_BYTES
        large = body - PACKING_GAP_BYTES
        lengths = dict(a=large - XATTR_ENTRY_BYTES - XATTR_ALIGNMENT,
                       b=body // 2 - XATTR_ENTRY_BYTES - XATTR_ALIGNMENT,
                       c=body // 2 - XATTR_ENTRY_BYTES - XATTR_ALIGNMENT,
                       large=capacity - large - XATTR_ENTRY_BYTES - 2 * XATTR_ALIGNMENT)
        for name, size in lengths.items():
            values["/plain", 1, name] = changed_value(size)
        objects["/plain"]["blocks"] += sectors
        counts["Free blocks"] -= 1
    elif operation == "full":
        for path in ("/block", "/shared", "/symlink"):
            del values[path, 1, "binary"]
            objects[path]["blocks"] -= sectors
        values["/capacity", 1, "full"] = changed_value(fixture["largest_value"])
        values["/mapped-symlink", 1, "binary"] = changed_value(700)
        if fixture["inode_size"] != INODE_BASE_BYTES:
            values["/block", 1, "tiny"] = values["/plain", 1, "tiny"] = b"abc"
        counts["Free blocks"] += 2
    else:
        raise RuntimeError(f"Unknown operation: {operation}")
    return objects, values, counts


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixtures", type=Path, required=True)
    parser.add_argument("--exports", type=Path, required=True)
    parser.add_argument("--recover", type=Path, required=True)
    parser.add_argument("--tools-root", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--discard-recovered", action="store_true",
                        help="Remove verified recovery copies after recording their hashes; preserve all input images")
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument("--edges", action="store_true")
    modes.add_argument("--full", action="store_true")
    args = parser.parse_args()
    tools = resolve_tools(args.tools_root)
    fixtures = json.loads(args.fixtures.read_text())
    if not fixtures or not all(item.get("passed") for item in fixtures):
        raise RuntimeError("Expected independently verified xattr fixtures")
    if args.full and not all(item.get("full_space") for item in fixtures):
        raise RuntimeError("Expected completely allocated xattr fixtures")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    exports = args.exports.resolve()
    records = []

    def save():
        (output / "report.json").write_text(json.dumps(records, indent=2) + "\n")

    for fixture in fixtures:
        source = Path(fixture["image"])
        expected = Path(fixture["expected"])
        inputs = {operation: exports / f"xattr-write-{operation}-{source.name}"
                  for operation in OPERATIONS}
        inputs["references"] = exports / f"xattr-references-{source.name}"
        interrupted = {(kind, operation): exports / f"xattr-{kind}-{operation}-{source.name}"
                       for operation in OPERATIONS for kind in ("pending", "uncommitted")}
        if args.edges:
            inputs = {"capacity": exports / f"xattr-capacity-{source.name}"}
            if fixture["inode_size"] != INODE_BASE_BYTES:
                inputs["packed"] = exports / f"xattr-packed-{source.name}"
            interrupted = {}
        elif args.full:
            inputs = {"full": exports / f"xattr-full-written-{source.name}"}
            interrupted = {}
        protected = {path: digest(path) for path in (source, expected, *inputs.values(), *interrupted.values())}
        record = dict(source=str(source), source_sha256=protected[source], states={}, commands=[])
        records.append(record)
        save()

        def require(condition, message):
            if not condition:
                save()
                raise RuntimeError(f"{source.name}: {message}")

        def run(command, allowed=(0,)):
            done = subprocess.run([str(part) for part in command], capture_output=True,
                                  text=True, errors="backslashreplace", timeout=120)
            record["commands"].append(dict(command=[str(part) for part in command],
                                           status=done.returncode, stdout=done.stdout, stderr=done.stderr))
            require(done.returncode in allowed, f"Independent command failed: {command}")
            return done.stdout

        require(protected[source] == fixture["input_sha256"] and
                protected[expected] == fixture["expected_sha256"], "Fixture changed before inspection")
        values = {}
        for line in expected.read_text().splitlines():
            path, namespace, name, payload = line.split()
            namespace = int(namespace)
            if namespace < 0:
                continue
            value_path = expected.parent / payload
            protected[value_path] = digest(value_path)
            name = "" if name == "-" else bytes.fromhex(name).decode("utf-8")
            values[path, namespace, name] = value_path.read_bytes()
        counts = accounting(run([tools["dumpe2fs"], "-h", source]))
        if args.edges and fixture["inode_size"] != INODE_BASE_BYTES:
            plain = run([tools["debugfs"], "-R", "stat /plain", source])
            extra = re.search(r"^Size of extra inode fields:\s+(\d+)", plain, re.M)
            require(extra is not None, "Missing independently decoded inode extra size")
            fixture["plain_extra_size"] = int(extra[1])

        def filler_digest(image):
            listing = run([tools["debugfs"], "-R", "blocks /filler", image])
            require(re.fullmatch(r"(?:\d+\s+)+", listing) is not None, "Missing filler block mapping")
            blocks = [int(number) for number in listing.split()]
            checksum = hashlib.sha256()
            with image.open("rb") as stream:
                for block in blocks:
                    require(0 < block < counts["Block count"], "Filler block outside resource")
                    stream.seek(block * fixture["block_size"])
                    data = stream.read(fixture["block_size"])
                    require(len(data) == fixture["block_size"], "Truncated filler block")
                    checksum.update(data)
            return dict(blocks=len(blocks), mapping_sha256=hashlib.sha256(listing.encode()).hexdigest(),
                        data_sha256=checksum.hexdigest())

        def inspect(image, state_name, wanted):
            objects, target, wanted_counts = wanted
            run([tools["e2fsck"], "-fn", image])
            actual_counts = accounting(run([tools["dumpe2fs"], "-h", image]))
            require(actual_counts == wanted_counts, f"Allocation accounting differs: {state_name}")
            blocks = {}
            headers = {}
            for path, inode in objects.items():
                stat = run([tools["debugfs"], "-R", f"stat {path}", image])
                actual = inode_fields(stat)
                require(canonical(actual) == inode, f"Inode differs: {state_name} {path}")
                match = re.search(r"^File ACL:\s+(\d+)", stat, re.M)
                require(match is not None, f"Missing independent xattr pointer: {path}")
                blocks[path] = int(match[1])
                if blocks[path] != 0:
                    with image.open("rb") as stream:
                        stream.seek(blocks[path] * fixture["block_size"])
                        header = dict(zip(XATTR_HEADER_FIELDS, XATTR_BLOCK_HEADER.unpack(stream.read(XATTR_BLOCK_HEADER.size))))
                    require(header["magic"] == XATTR_MAGIC and header["blocks"] == 1,
                            f"Invalid independent xattr block header: {path}")
                    headers[blocks[path]] = header
                listing = run([tools["debugfs"], "-R", f"ea_list {path}", image])
                entries = re.findall(r"^  (.+?) \((\d+)\)(?: =.*)?$", listing, re.M)
                wanted_names = {PREFIXES[namespace] + name: len(value)
                                for (owner, namespace, name), value in target.items() if owner == path}
                require(len(entries) == len(wanted_names) and
                        {listed_name(name): int(size) for name, size in entries} == wanted_names,
                        f"Attribute list differs: {state_name} {path}")
                if actual["type"] == "symlink":
                    data_inode = dict(actual)
                    data_inode["blocks"] -= (fixture["block_size"] // SECTOR_BYTES) * (blocks[path] != 0)
                    wanted_link = b"../plain" if path == "/symlink" else b"q" * 100
                    require(symlink_bytes(image, path, data_inode, fixture["block_size"], tools["debugfs"], run) == wanted_link,
                            f"Attribute mutation changed symlink bytes: {state_name} {path}")
            for block, header in headers.items():
                require(header["references"] == sum(value == block for value in blocks.values()),
                        f"Shared block reference count differs: {state_name} {block}")
            for index, ((path, namespace, name), value) in enumerate(target.items()):
                extracted = output / f"{source.stem}-{state_name}-{index}.data"
                query = f'ea_get -r -f "{extracted}" {path} "{PREFIXES[namespace]}{name}"'
                run([tools["debugfs"], "-R", query, image])
                require(extracted.is_file() and extracted.read_bytes() == value,
                        f"Attribute bytes differ: {state_name} {path} {namespace}:{name}")
            snapshot = dict(objects=objects, accounting=actual_counts, attribute_blocks=blocks,
                            headers=headers, values=len(target), clean_e2fsck=True)
            if args.full:
                snapshot["filler"] = filler_digest(image)
            record["states"][state_name] = dict(image=str(image), sha256=digest(image), passed=True, **snapshot)
            save()
            return snapshot

        baseline = (fixture["objects"], values, counts)
        old_snapshot = inspect(source, "before", baseline)
        for operation, image in inputs.items():
            wanted = expected_state(fixture, values, counts, operation)
            after = inspect(image, operation, wanted)
            if args.full:
                require(after["filler"] == old_snapshot["filler"], "Xattr mutation changed filler data or mapping")
            if operation == "references" or not interrupted:
                continue
            for kind in ("pending", "uncommitted"):
                original = interrupted[kind, operation]
                outcome = wanted if kind == "pending" else baseline
                snapshot = after if kind == "pending" else old_snapshot
                for backend in ("core", "oracle"):
                    recovered = output / f"{backend}-{original.name}"
                    shutil.copyfile(original, recovered)
                    if backend == "core":
                        replay = run([args.recover.resolve(), "--write", recovered])
                        require(f"transactions={1 if kind == 'pending' else 0} " in replay,
                                f"Unexpected committed transaction count: {original.name}")
                    else:
                        run([tools["e2fsck"], "-y", "-E", "journal_only", recovered], allowed=(0, 1))
                    state_name = f"{backend}-{kind}-{operation}"
                    require(inspect(recovered, state_name, outcome) == snapshot,
                            f"Recovery changed block sharing or metadata: {state_name}")
                    if backend == "core":
                        clean_sha = digest(recovered)
                        run([args.recover.resolve(), "--write", recovered])
                        require(digest(recovered) == clean_sha, "Repeated recovery changed a clean image")
                    if args.discard_recovered:
                        recovered.unlink()
        require(all(digest(path) == before for path, before in protected.items()), "Inspection modified a protected input")
        record.update(passed=True, protected_inputs_unchanged=True,
                      protected_files=[dict(path=str(path), sha256=value) for path, value in protected.items()])
        save()
        print(f"PASS {source.name}: {len(record['states'])} clean xattr states, exact values/metadata and independent replay", flush=True)


if __name__ == "__main__":
    main()
