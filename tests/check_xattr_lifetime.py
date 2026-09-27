#!/usr/bin/env python3
"""Check attribute lifetime, namespace and crash exports with independent tools."""

import argparse
import copy
import json
from pathlib import Path
import re
import shutil
import subprocess

from check_namespace import INODE_EXTENTS, inode_fields, symlink_bytes
from check_orphans import accounting, digest
from check_xattrs import PREFIXES, listed_name
from generate_fixtures import resolve_tools
from generate_xattr_fixtures import XATTR_BLOCK_HEADER, XATTR_HEADER_FIELDS, XATTR_MAGIC

SECONDS = 1700000080
SECTOR_BYTES = 512
DATA_BLOCKS = 40
CHILD_NAMES = ("child-file", "child-directory", "child-fast", "child-mapped")
OPERATIONS = ("create-file", "create-directory", "create-fast", "create-mapped", "write",
              "truncate", "unlink-shared", "unlink-fast", "unlink-mapped", "rmdir", "rename")
RELEASE_KINDS = ("shared", "fast", "mapped", "directory", "large")
ENABLE_OPERATIONS = ("create-file", "create-directory", "create-fast", "create-mapped",
                     "set-body", "set-external", "write", "truncate")
SECURITY_VALUE = b"system_u:object_r:tmp_t:s0\0"


def canonical(value):
    return json.loads(json.dumps(value))


def changed_value(size):
    return bytes((index * 17 + 0x51) & 255 for index in range(size))


def wanted_state(fixture, original, values, counts, state):
    objects = copy.deepcopy(original)
    values = dict(values)
    counts = dict(counts)
    contents = {"/symlink": b"../plain", "/mapped-symlink": b"q" * 100}
    sectors = fixture["block_size"] // SECTOR_BYTES

    def times(path, change_only=False):
        objects[path]["ctime"] = [SECONDS, 0]
        if not change_only:
            objects[path]["mtime"] = [SECONDS, 0]

    def remove(path, freed):
        del objects[path]
        contents.pop(path, None)
        for key in list(values):
            if key[0] == path:
                del values[key]
        counts["Free inodes"] += 1
        counts["Free blocks"] += freed

    def create(kind, parent, combined=False):
        path = parent.rstrip("/") + "/" + CHILD_NAMES[kind]
        directory = kind == 1
        symlink = kind >= 2
        data_blocks = int(kind in (1, 3))
        objects[path] = dict(type="directory" if directory else "symlink" if symlink else "regular",
                             mode=0o777 if combined and symlink else 0o761, uid=12345, gid=23456,
                             links=2 if directory else 1, size=fixture["block_size"] if directory else
                             13 if kind == 2 else 100 if kind == 3 else 0,
                             blocks=(1 + data_blocks) * sectors,
                             flags=0 if kind == 2 else original.get("/block", original["/plain"])["flags"],
                             atime=[SECONDS, 0], mtime=[SECONDS, 0], ctime=[SECONDS, 0],
                             crtime=None if fixture["inode_size"] == 128 else [0, 0])
        values[path, 1, "binary"] = changed_value(600 if combined else 700)
        if combined:
            values[path, 6, "selinux"] = SECURITY_VALUE
            if kind <= 1:
                values[path, 2, ""] = values["/directory", 3, ""]
            if directory:
                values[path, 3, ""] = values["/directory", 3, ""]
        if symlink:
            contents[path] = b"q" * objects[path]["size"]
        counts["Free inodes"] -= 1
        counts["Free blocks"] -= 1 + data_blocks
        if directory:
            objects[parent]["links"] += 1
        times(parent)

    if state == "before":
        return objects, values, counts, contents
    if state.startswith("enable-"):
        operation = state.removeprefix("enable-")
        if operation.startswith("create-"):
            create(ENABLE_OPERATIONS.index(operation), "/directory")
        else:
            path = "/plain"
            external = operation != "set-body" or fixture["inode_size"] == 128
            data_blocks = int(operation == "write")
            size = fixture["block_size"] + 20 if operation == "write" else 17 if operation == "truncate" else 0
            objects[path].update(mode=0o761, size=size, blocks=(int(external) + data_blocks) * sectors)
            values[path, 1, "binary"] = changed_value(3 if operation == "set-body" else 700)
            counts["Free blocks"] -= int(external) + data_blocks
            if operation == "write":
                contents[path] = bytes(fixture["block_size"] + 7) + changed_value(13)
            elif operation == "truncate":
                contents[path] = bytes(17)
            times(path)
    elif state == "created":
        for kind in range(4):
            create(kind, "/directory", combined=True)
    elif state == "created-removed":
        times("/directory")
        times("/")
    elif state in ("held-released", "held-recovered"):
        remove("/block", 0)
        remove("/plain", 0)
        times("/")
    elif state.startswith("release-"):
        kind = state.removeprefix("release-")
        path = {"shared": "/block", "fast": "/symlink", "mapped": "/mapped-symlink",
                "directory": "/directory", "large": "/block"}[kind]
        # Large-file data is allocated and freed by the test. Its final accounting
        # must equal the original empty inode's removal, including shared xattrs.
        remove(path, 0 if path == "/block" else objects[path]["blocks"] // sectors)
        remove("/plain", 0)
        if kind == "directory":
            objects["/"]["links"] -= 1
        times("/")
    elif state.startswith("renamed-child-"):
        kind = CHILD_NAMES.index(state.removeprefix("renamed-"))
        old = "/directory" if kind == 1 else "/block"
        new = "/" + CHILD_NAMES[kind]
        times(old, change_only=True)
        objects[new] = objects.pop(old)
        for key in list(values):
            if key[0] == old:
                values[new, key[1], key[2]] = values.pop(key)
        times("/")
    elif state.startswith("create-"):
        create(OPERATIONS.index(state), "/directory")
    elif state == "before-truncate":
        extra = 0 if objects["/block"]["flags"] & INODE_EXTENTS else 1
        objects["/block"].update(size=DATA_BLOCKS * fixture["block_size"],
                                  blocks=(DATA_BLOCKS + extra + 1) * sectors, mode=0o761)
        counts["Free blocks"] -= DATA_BLOCKS + extra
        contents["/block"] = b"\x5a" * objects["/block"]["size"]
        times("/block")
    elif state in ("write", "truncate"):
        objects["/block"].update(size=fixture["block_size"] + 20 if state == "write" else 17,
                                  blocks=2 * sectors, mode=0o761)
        values["/block", 1, "binary"] = changed_value(700)
        counts["Free blocks"] -= 2
        contents["/block"] = (bytes(fixture["block_size"] + 7) + changed_value(13)
                              if state == "write" else b"\x5a" * 17)
        times("/block")
    elif state in ("unlink-shared", "unlink-fast", "unlink-mapped", "rmdir", "rename"):
        path = {"unlink-shared": "/block", "unlink-fast": "/symlink",
                "unlink-mapped": "/mapped-symlink", "rmdir": "/directory", "rename": "/symlink"}[state]
        # The shared block remains owned by /shared. Other victims own every
        # block counted in i_blocks, including their external attributes.
        remove(path, 0 if state == "unlink-shared" else objects[path]["blocks"] // sectors)
        if state == "rmdir":
            objects["/"]["links"] -= 1
        elif state == "rename":
            times("/block", change_only=True)
            objects["/symlink"] = objects.pop("/block")
            for key in list(values):
                if key[0] == "/block":
                    values["/symlink", key[1], key[2]] = values.pop(key)
        times("/")
    else:
        raise RuntimeError(f"Unknown lifetime state: {state}")
    return objects, values, counts, contents


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixtures", type=Path, required=True)
    parser.add_argument("--exports", type=Path, required=True)
    parser.add_argument("--recover", type=Path, required=True)
    parser.add_argument("--tools-root", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--profile", action="append", help="Select an explicit fixture profile")
    parser.add_argument("--discard-recovered", action="store_true")
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument("--release", action="store_true",
                       help="verify final-release exports and interrupted orphan cleanup")
    modes.add_argument("--enable", action="store_true",
                       help="verify atomic first-attribute feature enablement")
    parser.add_argument("--keep-going", action="store_true",
                        help="retain failed replay images and continue; any failure still returns nonzero")
    args = parser.parse_args()
    fixtures = json.loads(args.fixtures.read_text())
    if args.profile:
        prefix = "xattr-enable-" if args.enable else "xattr-"
        fixtures = [f for f in fixtures if Path(f["image"]).stem.removeprefix(prefix) in args.profile]
        if len(fixtures) != len(set(args.profile)):
            raise RuntimeError("Every requested profile must have exactly one fixture")
    if not fixtures or not all(f.get("passed") for f in fixtures):
        raise RuntimeError("Expected independently accepted xattr fixtures")
    tools = resolve_tools(args.tools_root)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    exports = args.exports.resolve()
    records = []

    def save():
        (output / "report.json").write_text(json.dumps(records, indent=2) + "\n")

    for fixture in fixtures:
        source = Path(fixture["image"])
        protected = {source: digest(source)}
        record = dict(source=str(source), states={}, failed_replays={}, commands=[])
        records.append(record)

        def require(condition, message):
            if not condition:
                save()
                raise RuntimeError(f"{source.name}: {message}")

        def run(command, allowed=(0,)):
            done = subprocess.run([str(p) for p in command], capture_output=True,
                                  text=True, errors="backslashreplace", timeout=120)
            record["commands"].append(dict(command=[str(p) for p in command], status=done.returncode,
                                           stdout=done.stdout, stderr=done.stderr))
            require(done.returncode in allowed, f"Independent command failed: {command}")
            return done.stdout

        require(protected[source] == fixture["input_sha256"], "Fixture changed before inspection")
        values = {}
        if not args.enable:
            expected = Path(fixture["expected"])
            protected[expected] = digest(expected)
            require(protected[expected] == fixture["expected_sha256"], "Attribute fixture changed")
            for line in expected.read_text().splitlines():
                path, namespace, name, payload = line.split()
                namespace = int(namespace)
                if namespace < 0:
                    continue
                value_path = expected.parent / payload
                protected[value_path] = digest(value_path)
                values[path, namespace, "" if name == "-" else bytes.fromhex(name).decode("utf-8")] = value_path.read_bytes()
        original = copy.deepcopy(fixture["objects"])
        for path in ("/", "/lost+found"):
            original[path] = canonical(inode_fields(run([tools["debugfs"], "-R", f"stat {path}", source])))
        counts = accounting(run([tools["dumpe2fs"], "-h", source]))

        def inspect(image, label, state):
            objects, target, wanted_counts, contents = wanted_state(fixture, original, values, counts, state)
            run([tools["e2fsck"], "-fn", image])
            header = run([tools["dumpe2fs"], "-h", image])
            actual_counts = accounting(header)
            require(actual_counts == wanted_counts, f"Allocation accounting differs: {label}")
            if args.enable:
                feature_match = re.search(r"^Filesystem features:\s+(.+)$", header, re.M)
                wanted_features = set(fixture["features"]) | ({"ext_attr"} if state != "before" else set())
                require(feature_match is not None and set(feature_match[1].split()) == wanted_features,
                        f"First attribute feature transition differs: {label}")
            decoded = {}
            blocks = {}
            headers = {}
            for path, wanted in objects.items():
                stat = run([tools["debugfs"], "-R", f"stat {path}", image])
                actual = canonical(inode_fields(stat))
                require(actual is not None and all(actual[k] == v for k, v in wanted.items()),
                        f"Inode differs: {label} {path}; expected={wanted}, actual={actual}")
                if "inode" not in wanted:
                    require(actual["generation"] != 0 and actual["inode"] not in
                            {o["inode"] for o in original.values()}, f"Child reused an allocated identity: {path}")
                decoded[path] = actual
                match = re.search(r"^File ACL:\s+(\d+)", stat, re.M)
                require(match is not None, f"Missing external attribute pointer: {path}")
                blocks[path] = int(match[1])
                if blocks[path]:
                    with image.open("rb") as stream:
                        stream.seek(blocks[path] * fixture["block_size"])
                        header = dict(zip(XATTR_HEADER_FIELDS, XATTR_BLOCK_HEADER.unpack(stream.read(XATTR_BLOCK_HEADER.size))))
                    require(header["magic"] == XATTR_MAGIC and header["blocks"] == 1,
                            f"Invalid external attribute header: {path}")
                    headers[blocks[path]] = header
                listing = run([tools["debugfs"], "-R", f"ea_list {path}", image])
                entries = re.findall(r"^  (.+?) \((\d+)\)(?: =.*)?$", listing, re.M)
                names = {PREFIXES[index] + name: len(value)
                         for (owner, index, name), value in target.items() if owner == path}
                require(len(entries) == len(names) and {listed_name(n): int(s) for n, s in entries} == names,
                        f"Attribute names/sizes differ: {label} {path}")
                if actual["type"] == "symlink":
                    data_inode = dict(actual)
                    data_inode["blocks"] -= (fixture["block_size"] // SECTOR_BYTES) * bool(blocks[path])
                    require(symlink_bytes(image, path, data_inode, fixture["block_size"], tools["debugfs"], run) == contents[path],
                            f"Symlink bytes differ: {label} {path}")
                elif actual["type"] == "regular" and actual["size"]:
                    extracted = output / f"{source.stem}-{label}-data-{actual['inode']}"
                    run([tools["debugfs"], "-R", f'dump {path} "{extracted}"', image])
                    require(extracted.read_bytes() == contents[path], f"File data differs: {label} {path}")
            for block, header in headers.items():
                require(header["references"] == sum(b == block for b in blocks.values()),
                        f"Shared attribute reference count differs: {label} {block}")
            for index, ((path, namespace, name), value) in enumerate(target.items()):
                extracted = output / f"{source.stem}-{label}-attribute-{index}"
                run([tools["debugfs"], "-R", f'ea_get -r -f "{extracted}" {path} "{PREFIXES[namespace]}{name}"', image])
                require(extracted.is_file() and extracted.read_bytes() == value,
                        f"Attribute bytes differ: {label} {path} {namespace}:{name}")
            for path, inode in decoded.items():
                if inode["type"] != "directory" or path == "/lost+found":
                    continue
                parent = str(Path(path).parent)
                wanted = {".": inode["inode"], "..": decoded[parent]["inode"]}
                wanted.update({Path(p).name: i["inode"] for p, i in decoded.items()
                               if p != path and str(Path(p).parent) == path})
                listing = run([tools["debugfs"], "-R", f"ls -p {path}", image])
                entries = [line.split("/") for line in listing.splitlines() if line.strip()]
                found = {e[5]: int(e[1]) for e in entries if int(e[1]) != 0}
                require(found == wanted, f"Directory names/identities differ: {label} {path}: {found} != {wanted}")
            snapshot = dict(objects=decoded, accounting=actual_counts, attribute_blocks=blocks, headers=headers)
            record["states"][label] = dict(image=str(image), sha256=digest(image), passed=True,
                                           values=len(target), clean_e2fsck=True, **snapshot)
            save()
            return snapshot

        def exported(prefix):
            image = exports / f"xattr-{prefix}-{source.name}"
            protected[image] = digest(image)
            return image

        def replay(prefix, state, snapshot):
            pending = exported(prefix)
            for backend in ("core", "oracle"):
                image = output / f"{backend}-{pending.name}"
                shutil.copyfile(pending, image)
                label = f"{backend}-{prefix}"
                try:
                    if backend == "core":
                        run([args.recover.resolve(), "--write", image])
                    else:
                        run([tools["e2fsck"], "-y", "-E", "journal_only", image], allowed=(0, 1))
                    require(inspect(image, label, state) == snapshot,
                            f"Recovery changed expected block ownership: {prefix} {backend}")
                    if backend == "core":
                        before = digest(image)
                        run([args.recover.resolve(), "--write", image])
                        require(digest(image) == before, "Repeated recovery changed clean image")
                except RuntimeError as error:
                    record["states"].pop(label, None)
                    record["failed_replays"][label] = dict(
                        image=str(image), sha256=digest(image), passed=False, error=str(error))
                    save()
                    if not args.keep_going:
                        raise
                    print(f"FAIL {source.name}: {label}: {error}", flush=True)
                    continue
                if args.discard_recovered:
                    image.unlink()

        baseline = inspect(source, "before", "before")
        if args.enable:
            for operation in ENABLE_OPERATIONS:
                state = "enable-" + operation
                snapshot = inspect(exported(state), state, state)
                replay("enable-pending-" + operation, state, snapshot)
                replay("enable-uncommitted-" + operation, "before", baseline)
        elif args.release:
            for kind in RELEASE_KINDS:
                state = "release-" + kind
                snapshot = inspect(exported(state), state, state)
                for transition in ("pending", "uncommitted", "committed"):
                    replay(f"release-{transition}-{kind}", state, snapshot)
        else:
            for state in ("created", "created-removed", "held-released", "held-recovered",
                          *("renamed-" + n for n in CHILD_NAMES)):
                snapshot = inspect(exported(state), state, state)
                if state == "held-released":
                    replay("held-pending", "held-released", snapshot)
            for operation in OPERATIONS:
                before_state = "before-truncate" if operation == "truncate" else "before"
                before = inspect(exported("lifetime-before-" + operation), "before-" + operation, before_state)
                after = inspect(exported("lifetime-" + operation), operation, operation)
                replay("lifetime-pending-" + operation, operation, after)
                replay("lifetime-uncommitted-" + operation, before_state, before)
        require(all(digest(p) == h for p, h in protected.items()), "Protected image or value changed")
        record.update(passed=not record["failed_replays"], protected_inputs_unchanged=True,
                      protected_files=[dict(path=str(p), sha256=h) for p, h in protected.items()])
        save()
        status = "FAIL" if record["failed_replays"] else "PASS"
        print(f"{status} {source.name}: {len(record['states'])} successful independent lifetime states; "
              f"{len(record['failed_replays'])} failed replays", flush=True)
    if any(not record["passed"] for record in records):
        raise SystemExit(1)


if __name__ == "__main__":
    main()
