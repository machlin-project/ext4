#!/usr/bin/env python3
"""Check inline mutations through e2fsprogs, exact contents and nonrepairing fsck."""

import argparse
import json
from pathlib import Path
import re
import subprocess

from check_namespace import inode_fields
from check_orphans import accounting, digest
from generate_fixtures import resolve_tools
from generate_inline_fixtures import INLINE_DATA_FLAG, SIZES, payload

STATES = ("kept", "converted", "truncate-grown", "truncate-zero", "reserved", "xattrs",
          "directory-kept", "directory-expanded", "new", "lifetime")
SECONDS = 1700000200


def expected(state, block_size):
    files = {f"/file{size}": payload(size) for size in SIZES}
    files.update({f"/entries/e{index}": payload(index + 1) for index in range(6)})
    directories = {"/": {".", "..", "lost+found", "entries", "empty", "destination"} |
                   {f"file{size}" for size in SIZES},
                   "/entries": {".", ".."} | {f"e{index}" for index in range(6)},
                   "/empty": {".", ".."}, "/destination": {".", ".."}}
    changed = None
    values = {}
    if state == "kept":
        value = bytearray(payload(120))
        value[56:68] = bytes([0xd7]) * 12
        value[59:] = bytes(61)
        value[50:70] = bytes(20)
        files["/file120"] = bytes(value)
        changed = "/file120"
    elif state == "converted":
        value = bytearray(block_size + 15)
        value[:120] = payload(120)
        value[block_size + 3:] = bytes([0xd7]) * 12
        files["/file120"] = bytes(value)
        changed = "/file120"
    elif state.startswith("truncate-"):
        files["/file60"] = b"" if state == "truncate-zero" else payload(60) + bytes(block_size + 7 - 60)
        changed = "/file60"
    elif state == "reserved":
        changed = "/file61"
    elif state == "xattrs":
        changed = "/file120"
        values = {"large": payload(block_size - 120), "small": payload(52)}
    elif state.startswith("directory-"):
        del files["/entries/e4"]
        files["/entries/new"] = b""
        directories["/entries"].remove("e4")
        directories["/entries"].add("new")
        directories["/"].remove("empty")
        del directories["/empty"]
        if state == "directory-expanded":
            for prefix in "abcd":
                name = prefix + "q" * 94
                files["/entries/" + name] = b""
                directories["/entries"].add(name)
    elif state == "new":
        files["/empty/file"] = payload(120)
        directories["/empty"].update(("file", "dir"))
        directories["/empty/dir"] = {".", ".."}
    elif state == "lifetime":
        del files["/file1"]
        directories["/"].remove("file1")
    return files, directories, changed, values


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixtures", type=Path, required=True)
    parser.add_argument("--exports", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--tools-root", type=Path)
    parser.add_argument("--full", action="store_true")
    args = parser.parse_args()
    fixtures = json.loads(args.fixtures.read_text())
    if not fixtures or not all(row.get("passed") for row in fixtures):
        raise RuntimeError("Unverified inline fixture input")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    tools = resolve_tools(args.tools_root)
    reports = []
    for fixture in fixtures:
        source = Path(fixture["image"])
        source_hash = digest(source)
        source_counts = fixture.get("accounting")
        for state in (("new", "lifetime") if args.full else STATES):
            image = args.exports.resolve() / f"inline-{state}-{source.name}"
            record = dict(image=str(image), source=str(source), state=state, commands=[])
            reports.append(record)

            def run(command):
                command = [str(item) for item in command]
                result = subprocess.run(command, capture_output=True, timeout=120)
                record["commands"].append(dict(command=command, status=result.returncode,
                                               stdout=result.stdout.decode("utf-8", "backslashreplace"),
                                               stderr=result.stderr.decode("utf-8", "backslashreplace")))
                result.check_returncode()
                return result.stdout.decode("utf-8")

            def debug(command):
                return run([tools["debugfs"], "-R", command, image])

            try:
                original_hash = digest(image)
                run([tools["e2fsck"], "-fn", image])
                files, directories, changed, values = expected(state, fixture["block_size"])
                if args.full:
                    directories["/"].add("filler")
                    original = output / f"{image.stem}-source-filler.data"
                    run([tools["debugfs"], "-R", f'dump /filler "{original}"', source])
                    files["/filler"] = original.read_bytes()
                for path, value in sorted(files.items()):
                    fields = inode_fields(debug(f"stat {path}"))
                    if fields["size"] != len(value) or fields["type"] != "regular":
                        raise RuntimeError(f"Wrong file type/EOF: {path}")
                    target = output / f"{image.stem}-{path[1:].replace('/', '_')}.data"
                    debug(f'dump {path} "{target}"')
                    observed = target.read_bytes()
                    # libext2fs returns the storage capacity for inline data.
                    if fields["flags"] & INLINE_DATA_FLAG:
                        if len(observed) < len(value) or any(observed[len(value):]):
                            raise RuntimeError(f"Wrong inline capacity/padding: {path}")
                        observed = observed[:len(value)]
                    if observed != value:
                        raise RuntimeError(f"File bytes differ: {path}")
                    if path == changed and (fields["mode"] != 0o640 or
                                           fields["mtime"] != (SECONDS, 0) or
                                           fields["ctime"] != (SECONDS, 0)):
                        raise RuntimeError(f"Mutation metadata differs: {path}")
                    if (path in ("/entries/new", "/empty/file") or
                            (path.startswith("/entries/") and len(path.rsplit("/", 1)[1]) == 95)) and (
                            fields["uid"] != 70000 or fields["gid"] != 80000 or
                            fields["mode"] != 0o640 or fields["links"] != 1):
                        raise RuntimeError(f"Created inode metadata differs: {path}")
                    if path == changed and state == "kept" and (
                            not fields["flags"] & INLINE_DATA_FLAG or fields["blocks"] != 0):
                        raise RuntimeError("Inline overwrite/shrink/punch allocated file blocks")
                    if path == changed and state in ("converted", "truncate-grown", "reserved") and (
                            fields["flags"] & INLINE_DATA_FLAG):
                        raise RuntimeError("Growing operation retained an undersized inline representation")
                    if path == changed and state == "truncate-zero" and fields["blocks"] != 0:
                        raise RuntimeError("Truncate leaked data blocks")
                for path, names in sorted(directories.items()):
                    observed = set(re.findall(r"^/\d+/\d+/\d+/\d+/([^/]+)/", debug(f"ls -p {path}"), re.M))
                    if observed != names:
                        raise RuntimeError(f"Directory names differ: {path}: {observed} != {names}")
                    fields = inode_fields(debug(f"stat {path}"))
                    if path == "/entries" and state.startswith("directory-") and (
                            bool(fields["flags"] & INLINE_DATA_FLAG) != (state == "directory-kept")):
                        raise RuntimeError("Unexpected directory conversion")
                names = re.findall(r"^\s+user\.([^\s]+)\s+\(\d+\)", debug("ea_list /file120"), re.M)
                if sorted(names) != sorted(values):
                    raise RuntimeError("User attribute set changed unexpectedly")
                for name, value in values.items():
                    target = output / f"{image.stem}-{name}.value"
                    debug(f'ea_get -r -f "{target}" /file120 user.{name}')
                    if target.read_bytes() != value:
                        raise RuntimeError(f"Wrong xattr bytes: {name}")
                counts = accounting(run([tools["dumpe2fs"], "-h", image]))
                if state in ("new", "lifetime"):
                    if source_counts is None:
                        source_counts = accounting(run([tools["dumpe2fs"], "-h", source]))
                    if (counts["Free blocks"] != source_counts["Free blocks"] or
                            counts["Free inodes"] != source_counts["Free inodes"] +
                            (1 if state == "lifetime" else -2)):
                        raise RuntimeError("Inline creation/lifetime allocation differs")
                    if state == "new":
                        for path in ("/empty/file", "/empty/dir"):
                            created = inode_fields(debug(f"stat {path}"))
                            if (not created["flags"] & INLINE_DATA_FLAG or created["blocks"] != 0 or
                                    created["uid"] != 70000 or created["gid"] != 80000 or
                                    created["mode"] != 0o640 or
                                    any(created[key] != (SECONDS, 0) for key in ("atime", "mtime", "ctime"))):
                                raise RuntimeError("Created inode representation or metadata differs")
                if original_hash != digest(image) or source_hash != digest(source):
                    raise RuntimeError("Read-only verification changed media")
                record.update(passed=True, image_sha256=original_hash, source_sha256=source_hash,
                              allocation=counts, files=len(files), directories=len(directories))
                print(f"PASS {image.name}: exact data, namespace, attributes and clean fsck", flush=True)
            except Exception as error:
                record.update(passed=False, error=str(error))
                raise
            finally:
                (output / "report.json").write_text(json.dumps(reports, indent=2) + "\n")


if __name__ == "__main__":
    main()
