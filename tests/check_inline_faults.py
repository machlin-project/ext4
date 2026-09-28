#!/usr/bin/env python3
"""Compare inline transactions with portable and independent journal replay."""

import argparse
import copy
import json
from pathlib import Path
import re
import shutil
import subprocess

from generate_inline_fixtures import payload
from check_inline import SECONDS
from check_orphans import digest
from generate_fixtures import resolve_tools
from linux_xattrs import snapshot

OPERATIONS = ("overwrite", "grow", "convert", "add", "directory-grow", "attributes", "unlink")


def logical_objects(state):
    objects = copy.deepcopy(state["objects"])
    for item in objects.values():
        item["attrs"].pop("system.data", None)
    return objects


def verify_transition(before, after, operation):
    expected = logical_objects(before)
    observed = logical_objects(after)
    block_size = before["accounting"]["Block size"]
    if operation in ("add", "directory-grow"):
        name = "x" * (3 if operation == "add" else 255)
        path = "/empty/" + name
        created = observed[path]
        wanted = dict(mode=0o640, uid=70000, gid=80000, links=1, size=0, type="regular",
                      atime=(SECONDS, 0), ctime=(SECONDS, 0), mtime=(SECONDS, 0))
        if any(created["inode"][key] != value for key, value in wanted.items()) or created["data"] or created["attrs"]:
            raise RuntimeError("Wrong created inline inode")
        if created["inode"]["inode"] in {x["inode"]["inode"] for x in expected.values()}:
            raise RuntimeError("Creation reused an allocated inode")
        expected[path] = created
        expected["/empty"]["names"][name] = created["inode"]["inode"]
        expected["/empty"]["inode"].update(ctime=(SECONDS, 0), mtime=(SECONDS, 0))
        for key in ("size", "blocks", "flags"):
            expected["/empty"]["inode"][key] = observed["/empty"]["inode"][key]
    elif operation == "unlink":
        del expected["/file120"]
        del expected["/"]["names"]["file120"]
        expected["/"]["inode"].update(ctime=(SECONDS, 0), mtime=(SECONDS, 0))
    else:
        path = "/file1" if operation == "grow" else "/file120"
        item = expected[path]
        value = bytearray.fromhex(item["data"])
        if operation == "overwrite":
            value[:31] = bytes([0xe4]) * 31
        elif operation == "grow":
            value = bytearray(payload(120))
        elif operation == "convert":
            value += bytes(block_size + 127 - len(value))
            value[block_size + 7:] = payload(120)
        elif operation == "attributes":
            item["attrs"].update({"user.large": payload(block_size - 120).hex(),
                                  "user.small": payload(52).hex()})
        item["data"] = value.hex()
        item["inode"].update(mode=0o640, size=len(value), ctime=(SECONDS, 0), mtime=(SECONDS, 0))
        if operation in ("convert", "attributes"):
            for key in ("blocks", "flags"):
                item["inode"][key] = observed[path]["inode"][key]
    if expected != observed:
        raise RuntimeError(f"Unexpected inline transaction result: {operation}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exports", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--recover", required=True, type=Path)
    parser.add_argument("--tools-root", type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    tools = resolve_tools(args.tools_root)
    recover = args.recover.resolve()
    recover_hash = digest(recover)
    records = []
    selected = []
    for profile in ("1k", "4k"):
        for operation in OPERATIONS:
            states = {}
            for kind, replay, wanted in (("before", None, "before"), ("after", None, "after"),
                                        ("pending", "core", "after"), ("pending", "oracle", "after"),
                                        ("uncommitted", "core", "before"),
                                        ("uncommitted", "oracle", "before")):
                source = args.exports.resolve() / f"inline-fault-{kind}-{operation}-inline-{profile}.img"
                image = output / f"{replay or 'clean'}-{source.name}"
                record = dict(source=str(source), image=str(image), profile=profile,
                              operation=operation, kind=kind, replay=replay, commands=[])
                records.append(record)

                def run(command, allowed=(0,)):
                    done = subprocess.run([str(x) for x in command], capture_output=True,
                                          text=True, errors="backslashreplace", timeout=120)
                    record["commands"].append(dict(command=[str(x) for x in command],
                                                    status=done.returncode, stdout=done.stdout,
                                                    stderr=done.stderr))
                    if done.returncode not in allowed:
                        raise RuntimeError(f"inline independent command failed: {command}")
                    return done.stdout

                try:
                    original_hash = digest(source)
                    shutil.copyfile(source, image)
                    if replay == "core":
                        text = run([recover, "--write", image])
                        transactions = re.search(r"transactions=(\d+)", text)
                        if transactions is None or int(transactions[1]) != int(kind == "pending"):
                            raise RuntimeError("inline transaction visibility differs")
                    elif replay == "oracle":
                        text = run([tools["e2fsck"], "-y", "-E", "journal_only", image])
                        if re.search(r"^Pass [1-5]:", text, re.M):
                            raise RuntimeError("inline oracle unexpectedly entered full repair")
                    state = snapshot(image, output / f"{image.stem}-state", tools, run)
                    if replay is None:
                        states[kind] = state
                        if kind == "after":
                            verify_transition(states["before"], state, operation)
                    elif state != states[wanted]:
                        raise RuntimeError(f"inline {replay} replay differs from the {wanted} state")
                    clean_hash = digest(image)
                    if replay == "core":
                        run([recover, "--write", image])
                    if (digest(image) != clean_hash or digest(source) != original_hash or
                            digest(recover) != recover_hash):
                        raise RuntimeError("inline recovery/checking changed a protected artifact")
                    record.update(passed=True, source_sha256=original_hash, image_sha256=clean_hash)
                    if kind == "pending" and replay == "core" and (
                            (profile, operation) in (("1k", "directory-grow"), ("4k", "convert"))):
                        selected.append(dict(image=str(image), input_sha256=clean_hash,
                                             pending=str(source), pending_sha256=original_hash,
                                             recovered_outcome="new", passed=True, inline_data=True,
                                             block_size=state["accounting"]["Block size"]))
                    else:
                        image.unlink()
                    print(f"PASS {profile} {operation} {kind} {replay or 'clean'}", flush=True)
                except Exception as error:
                    record.update(passed=False, error=str(error))
                    raise
                finally:
                    (output / "report.json").write_text(json.dumps(records, indent=2) + "\n")
    (output / "linux-pending.json").write_text(json.dumps(selected, indent=2) + "\n")


if __name__ == "__main__":
    main()
