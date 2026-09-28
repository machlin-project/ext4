#!/usr/bin/env python3
"""Compare cluster transactions with portable and independent journal replay."""

import argparse
import copy
import json
from pathlib import Path
import re
import shutil
import subprocess

from generate_cluster_fixtures import payload
from check_clusters import SECONDS
from check_orphans import digest
from generate_fixtures import resolve_tools
from linux_xattrs import SECTOR_BYTES, snapshot

OPERATIONS = ("reuse", "allocate", "retain", "release", "reserve", "attribute-remove")


def verify_transition(before, after, operation, ratio):
    expected = copy.deepcopy(before)
    block = before["accounting"]["Block size"]
    cluster = block * ratio
    item = expected["objects"]["/file" if operation == "attribute-remove" else "/sparse"]
    delta = 0
    value = bytearray.fromhex(item["data"])
    if operation in ("reuse", "allocate"):
        offset = 2 * block if operation == "reuse" else 2 * cluster + block
        value[offset:offset + block] = payload(block)
        delta = int(operation == "allocate")
    elif operation in ("retain", "release"):
        offset = 0 if operation == "retain" else 9 * cluster
        value[offset:offset + block] = bytes(block)
        delta = -int(operation == "release")
    elif operation == "reserve":
        delta = 1
    elif operation == "attribute-remove":
        del item["attrs"]["user.value"]
        delta = -1
    item["data"] = value.hex()
    item["inode"].update(mode=0o640, ctime=(SECONDS, 0), mtime=(SECONDS, 0))
    item["inode"]["blocks"] += delta * cluster // SECTOR_BYTES
    expected["accounting"]["Free blocks"] -= delta * ratio
    if expected != after:
        raise RuntimeError(f"Unexpected cluster transaction result: {operation}")


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
                source = args.exports.resolve() / f"cluster-fault-{kind}-{operation}-cluster-{profile}.img"
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
                        raise RuntimeError(f"cluster independent command failed: {command}")
                    return done.stdout

                try:
                    original_hash = digest(source)
                    shutil.copyfile(source, image)
                    if replay == "core":
                        text = run([recover, "--write", image])
                        transactions = re.search(r"transactions=(\d+)", text)
                        if transactions is None or int(transactions[1]) != int(kind == "pending"):
                            raise RuntimeError("cluster transaction visibility differs")
                    elif replay == "oracle":
                        text = run([tools["e2fsck"], "-y", "-E", "journal_only", image])
                        if re.search(r"^Pass [1-5]:", text, re.M):
                            raise RuntimeError("cluster oracle unexpectedly entered full repair")
                    state = snapshot(image, output / f"{image.stem}-state", tools, run)
                    if replay is None:
                        states[kind] = state
                        if kind == "after":
                            verify_transition(states["before"], state, operation, 4)
                    elif state != states[wanted]:
                        raise RuntimeError(f"cluster {replay} replay differs from the {wanted} state")
                    clean_hash = digest(image)
                    if replay == "core":
                        run([recover, "--write", image])
                    if (digest(image) != clean_hash or digest(source) != original_hash or
                            digest(recover) != recover_hash):
                        raise RuntimeError("cluster recovery/checking changed a protected artifact")
                    record.update(passed=True, source_sha256=original_hash, image_sha256=clean_hash)
                    if kind == "pending" and replay == "core" and (
                            (profile, operation) in (("1k", "release"), ("4k", "allocate"))):
                        selected.append(dict(image=str(image), input_sha256=clean_hash,
                                             pending=str(source), pending_sha256=original_hash,
                                             recovered_outcome="new", passed=True, clustered=True,
                                             cluster_blocks=4,
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
