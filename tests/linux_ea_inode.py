"""Verify Linux/core/Linux large-value sharing and orphan recovery."""

import copy
import re
import shutil

from check_orphans import digest
from linux_xattrs import (payload, returned_payload, snapshot,
                          write_core_expectations, RETURN_SECONDS)

VALUE_MAX = 65536
FILE = "/linux-ea-file"
DIRECTORY = "/linux-ea-directory"


def expected_linux(before):
    objects = copy.deepcopy(before["objects"])
    objects["/body"]["attrs"]["user.maximum"] = payload(VALUE_MAX).hex()
    del objects["/many"]["attrs"]["user.value7"]
    objects["/many"]["attrs"]["user.value0"] = payload(VALUE_MAX).hex()
    objects[FILE] = dict(inode=dict(type="regular", mode=0o600, uid=0, gid=0,
                                  links=1, size=17), data=payload(17).hex(), attrs={
        "user.maximum": payload(VALUE_MAX).hex(), "user.duplicate": payload(VALUE_MAX).hex(),
        "user.small": payload(13).hex(), "user.empty": ""})
    objects[DIRECTORY] = dict(inode=dict(type="directory", mode=0o750, uid=0, gid=0,
                                       links=2), attrs={
        "user.medium": payload(3 * before["accounting"]["Block size"] + 7).hex()})
    objects["/"]["inode"]["links"] += 1
    return objects


def verify_expected(actual, expected, *, returned=False):
    if set(actual["objects"]) != set(expected):
        raise RuntimeError("EA_INODE namespace differs, including orphan cleanup")
    ignored = ({FILE: {"blocks"}} if returned else {
        "/body": {"ctime", "blocks"}, "/many": {"ctime", "blocks"},
        "/": {"ctime", "mtime", "size", "blocks"}})
    for path, wanted in expected.items():
        item = actual["objects"][path]
        if item["attrs"] != wanted["attrs"] or item.get("data") != wanted.get("data"):
            raise RuntimeError(f"EA_INODE names, values or contents differ: {path}")
        for field, value in wanted["inode"].items():
            if field not in ignored.get(path, set()) and item["inode"].get(field) != value:
                raise RuntimeError(f"EA_INODE owner field differs: {path} {field}")
        if "names" in wanted:
            names = dict(wanted["names"])
            if not returned and path == "/":
                for child in (FILE, DIRECTORY):
                    names[child[1:]] = actual["objects"][child]["inode"]["inode"]
            if names != item["names"]:
                raise RuntimeError(f"EA_INODE directory entries differ: {path}")
    if not returned:
        old = {item["inode"]["inode"] for path, item in expected.items()
               if path not in (FILE, DIRECTORY)}
        new = {actual["objects"][path]["inode"]["inode"] for path in (FILE, DIRECTORY)}
        if len(new) != 2 or old & new:
            raise RuntimeError("Linux reused an allocated EA_INODE owner")
        directory = actual["objects"][DIRECTORY]
        if directory["names"] != {".": directory["inode"]["inode"],
                                  "..": actual["objects"]["/"]["inode"]["inode"]}:
            raise RuntimeError("EA_INODE directory parent differs")


def verify(case, image, output, tools, recover, reader, run, native_replay):
    if native_replay is None:
        raise RuntimeError("EA_INODE orphan recovery requires the native Linux reference")
    oracle = output / f"linux-reference-{image.name}"
    pending = output / f"linux-pending-{image.name}"
    shutil.copyfile(image, pending)
    pending_hash = digest(pending)
    shutil.copyfile(image, oracle)
    replay = run([recover, "--write", image])
    transactions = re.search(r"transactions=(\d+)", replay)
    orphans = re.search(r"orphans=(\d+)", replay)
    if transactions is None or int(transactions[1]) == 0:
        raise RuntimeError("EA_INODE reverse roundtrip has no Linux transaction")
    if orphans is None or int(orphans[1]) == 0:
        raise RuntimeError("EA_INODE reverse roundtrip did not clean a Linux orphan")
    states = []
    for label, candidate in (("core", image), ("oracle", oracle)):
        if label == "oracle":
            # e2fsprogs 1.47.3 journal-only orphan cleanup leaves a shared
            # value's reference count stale. Preserve that failing report
            # separately and use Linux itself as this recovery oracle.
            native_replay(candidate, image, "replay")
        state = snapshot(candidate, output / f"{label}-{image.stem}-checked", tools, run)
        verify_expected(state, expected_linux(case["xattr_before"]))
        state.pop("oracle_summary_lag")
        states.append(state)
    if states[0] != states[1]:
        raise RuntimeError("Core and independent EA_INODE replay disagree")
    clean = digest(image)
    run([recover, "--write", image])
    if digest(image) != clean:
        raise RuntimeError("Repeated EA_INODE recovery changed the clean image")
    manifest = write_core_expectations(states[0], output / f"core-{image.stem}-values")
    run([reader, "--verify", image, manifest])
    returned = output / f"returned-{image.name}"
    run([reader, "--ea-roundtrip", returned, image, manifest])
    after = snapshot(returned, output / f"returned-{image.stem}-checked", tools, run)
    wanted = copy.deepcopy(states[0]["objects"])
    changed = wanted[FILE]
    changed["attrs"]["user.maximum"] = returned_payload(VALUE_MAX - 3).hex()
    changed["attrs"]["user.return"] = returned_payload(13).hex()
    del changed["attrs"]["user.duplicate"]
    del changed["attrs"]["user.empty"]
    changed["inode"].update(mode=0o640, uid=54321, gid=65432, ctime=(RETURN_SECONDS, 0))
    verify_expected(after, wanted, returned=True)
    if digest(pending) != pending_hash:
        raise RuntimeError("Linux-authored EA_INODE journal changed during verification")
    return dict(linux_authored_transactions=int(transactions[1]),
                linux_orphans_cleaned=int(orphans[1]), reverse_state=states[0],
                independent_replay="native Linux with strict nonrepairing e2fsck",
                linux_pending_image=str(pending), linux_pending_sha256=pending_hash,
                returned_image=str(returned), returned_sha256=digest(returned), returned_state=after)
