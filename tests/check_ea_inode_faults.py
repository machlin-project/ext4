#!/usr/bin/env python3
"""Compare interrupted EA_INODE transactions with portable and independent replay."""

import argparse
import copy
import json
from pathlib import Path
import re
import shutil
import subprocess

from check_ea_inode import changed, VALUE_MAX
from check_orphans import digest
from generate_fixtures import resolve_tools
from linux_xattrs import snapshot

OPERATIONS = ("create", "replace", "remove", "copy")
CHANGE_SECONDS = 1700000120


def verify_transition(before, after, operation):
    expected = copy.deepcopy(before["objects"])
    path = "/plain" if operation == "create" else "/alias" if operation == "copy" else "/body"
    item = expected[path]
    if operation in ("create", "replace"):
        item["attrs"]["user.maximum"] = changed(VALUE_MAX).hex()
    else:
        del item["attrs"]["user.value7" if operation == "copy" else "user.maximum"]
    item["inode"].update(mode=0o640, ctime=(CHANGE_SECONDS, 0), mtime=(CHANGE_SECONDS, 0))
    item["inode"]["blocks"] = after["objects"][path]["inode"]["blocks"]
    if expected != after["objects"]:
        raise RuntimeError(f"Unexpected EA_INODE transaction result: {operation}")


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
    for profile in ("1k", "inode128"):
        for operation in OPERATIONS:
            states = {}
            for kind, replay, wanted in (("before", None, "before"), ("after", None, "after"),
                                        ("pending", "core", "after"), ("pending", "oracle", "after"),
                                        ("uncommitted", "core", "before"),
                                        ("uncommitted", "oracle", "before")):
                source = args.exports.resolve() / f"ea-fault-{kind}-{operation}-ea-inode-{profile}.img"
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
                        raise RuntimeError(f"EA_INODE independent command failed: {command}")
                    return done.stdout

                try:
                    original_hash = digest(source)
                    shutil.copyfile(source, image)
                    if replay == "core":
                        text = run([recover, "--write", image])
                        transactions = re.search(r"transactions=(\d+)", text)
                        if transactions is None or int(transactions[1]) != int(kind == "pending"):
                            raise RuntimeError("EA_INODE transaction visibility differs")
                    elif replay == "oracle":
                        text = run([tools["e2fsck"], "-y", "-E", "journal_only", image])
                        if re.search(r"^Pass [1-5]:", text, re.M):
                            raise RuntimeError("EA_INODE oracle unexpectedly entered full repair")
                    state = snapshot(image, output / f"{image.stem}-state", tools, run)
                    if replay is None:
                        states[kind] = state
                        if kind == "after":
                            verify_transition(states["before"], state, operation)
                    elif state != states[wanted]:
                        raise RuntimeError(f"EA_INODE {replay} replay differs from the {wanted} state")
                    clean_hash = digest(image)
                    if replay == "core":
                        run([recover, "--write", image])
                    if (digest(image) != clean_hash or digest(source) != original_hash or
                            digest(recover) != recover_hash):
                        raise RuntimeError("EA_INODE recovery/checking changed a protected artifact")
                    record.update(passed=True, source_sha256=original_hash, image_sha256=clean_hash)
                    if kind == "pending" and replay == "core" and (
                            (profile, operation) in (("1k", "copy"), ("inode128", "replace"))):
                        selected.append(dict(image=str(image), input_sha256=clean_hash,
                                             pending=str(source), pending_sha256=original_hash,
                                             recovered_outcome="new", passed=True, ea_inode=True,
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
