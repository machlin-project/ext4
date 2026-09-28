#!/usr/bin/env python3
"""Independently check large-attribute values, sharing and inode reclamation."""

import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess

from check_orphans import accounting, digest
from generate_fixtures import resolve_tools
from generate_xattr_fixtures import payload

STATES = ("created", "shrunk", "replaced", "created-file", "created-directory",
          "created-fast", "created-mapped", "released-0", "released-1", "released-2",
          "released-3", "shared", "copied", "copy-released", "detached", "data-released",
          "value-orphan-pending", "value-orphan-clean")
VALUE_MAX = 65536


def changed(size):
    return bytes((index * 17 + 0x51) & 255 for index in range(size))


def expected(state, block_size):
    values = {("/body", "maximum"): payload(VALUE_MAX), ("/body", "small"): payload(13),
              ("/body", "empty"): b""}
    values.update({("/many", f"value{index}"): payload(3 * block_size + 7) for index in range(8)})
    paths = {"/body", "/many", "/plain", "/alias"}
    if state == "created":
        values.update({("/plain", key): changed(size) for key, size in
                       (("maximum", VALUE_MAX), ("medium", 3 * block_size + 7), ("small", 13))})
    elif state == "shrunk":
        values["/plain", "maximum"] = changed(13)
    elif state == "replaced":
        values["/body", "maximum"] = changed(VALUE_MAX - 3)
    elif state.startswith("created-"):
        path = "/" + state
        paths.add(path)
        values[path, "maximum"] = changed(13 if state == "created-fast" else VALUE_MAX)
        values[path, "medium"] = changed(13 if state == "created-fast" else 3 * block_size + 7)
    elif state in ("shared", "copied"):
        values.update({("/alias", f"value{index}"): payload(3 * block_size + 7)
                       for index in range(8 if state == "shared" else 7)})
    elif state in ("copy-released", "detached"):
        paths.remove("/alias")
    elif state == "data-released":
        paths.remove("/body")
        values = {key: value for key, value in values.items() if key[0] != "/body"}
    elif state.startswith("value-orphan-"):
        del values["/body", "maximum"]
    return paths, values


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixtures", type=Path, required=True)
    parser.add_argument("--exports", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--tools-root", type=Path)
    parser.add_argument("--state", choices=STATES, action="append",
                        help="Require only these selected states; defaults to every state")
    args = parser.parse_args()
    fixtures = json.loads(args.fixtures.read_text())
    if not fixtures or not all(item.get("passed") for item in fixtures):
        raise RuntimeError("Expected independently validated source fixtures")
    tools = resolve_tools(args.tools_root)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    records = []
    for fixture in fixtures:
        source = Path(fixture["image"])
        source_hash = digest(source)
        for state in (args.state or STATES):
            exported = args.exports.resolve() / f"ea-{state}-{source.name}"
            image = output / exported.name
            record = dict(image=str(image), exported=str(exported), source=str(source), state=state, commands=[])
            records.append(record)

            def run(command):
                result = subprocess.run([str(x) for x in command], capture_output=True, timeout=120)
                record["commands"].append(dict(command=[str(x) for x in command], status=result.returncode,
                                               stdout=result.stdout.decode("utf-8", "backslashreplace"),
                                               stderr=result.stderr.decode("utf-8", "backslashreplace")))
                result.check_returncode()
                return result.stdout.decode("utf-8")

            def debug(command):
                return run([tools["debugfs"], "-R", command, image])

            try:
                exported_hash = digest(exported)
                shutil.copyfile(exported, image)
                # Exports may be taken while the writer is attached. Clear its
                # recovery marker using journal replay only, never fsck repair.
                replay = run([tools["e2fsck"], "-y", "-E", "journal_only", image])
                if re.search(r"^Pass [1-5]:", replay, re.M):
                    raise RuntimeError("Journal-only replay unexpectedly entered filesystem repair")
                image_hash = digest(image)
                paths, values = expected(state, fixture["block_size"])
                for path in sorted(paths):
                    text = debug(f"stat {path}")
                    if re.search(r"^Inode:\s+\d+", text, re.M) is None:
                        raise RuntimeError(f"Missing object {path}")
                    listed = debug(f"ea_list {path}")
                    names = re.findall(r"^\s+user\.([^\s]+)\s+\(\d+\)", listed, re.M)
                    wanted = sorted(key for owner, key in values if owner == path)
                    if sorted(names) != wanted:
                        raise RuntimeError(f"Attribute names differ for {path}: {names} != {wanted}")
                    for name in wanted:
                        target = output / f"{image.stem}-{path[1:]}-{name}.value"
                        debug(f'ea_get -r -f "{target}" {path} user.{name}')
                        if target.read_bytes() != values[path, name]:
                            raise RuntimeError(f"Attribute value differs: {path} user.{name}")
                names = set(re.findall(r"^/\d+/\d+/\d+/\d+/([^/]+)/", debug("ls -p /"), re.M))
                if names != {".", "..", "lost+found"} | {path[1:] for path in paths}:
                    raise RuntimeError(f"Directory namespace differs: {names}")
                header = run([tools["dumpe2fs"], "-h", image])
                counts = accounting(header)
                source_counts = accounting(run([tools["dumpe2fs"], "-h", source]))
                if state.startswith("released-") and counts != source_counts:
                    raise RuntimeError("Reclamation did not return source allocation counters")
                if state in ("copy-released", "detached") and (
                        counts["Free blocks"] != source_counts["Free blocks"] or
                        counts["Free inodes"] != source_counts["Free inodes"] + 1):
                    raise RuntimeError("Shared attribute detachment changed physical ownership")
                if state.startswith("value-orphan-") and (
                        counts["Free inodes"] != source_counts["Free inodes"] + 1 or
                        counts["Free blocks"] < source_counts["Free blocks"] +
                        (VALUE_MAX + fixture["block_size"] - 1) // fixture["block_size"]):
                    raise RuntimeError("Private orphan did not release its inode and value data")
                run([tools["e2fsck"], "-fn", image])
                if (image_hash != digest(image) or source_hash != digest(source) or
                        exported_hash != digest(exported)):
                    raise RuntimeError("Read-only independent checks changed input media")
                record.update(passed=True, image_sha256=image_hash, source_sha256=source_hash,
                              exported_sha256=exported_hash, attributes=len(values), allocation=counts,
                              checked_copy_removed=True)
                image.unlink()
                print(f"PASS {image.name}: exact attributes, namespace, sharing and clean fsck", flush=True)
            except Exception as error:
                record.update(passed=False, error=str(error))
                raise
            finally:
                (output / "report.json").write_text(json.dumps(records, indent=2) + "\n")


if __name__ == "__main__":
    main()
