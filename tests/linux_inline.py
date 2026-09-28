"""Verify native Linux inline files, directories, orphan replay and core return."""

import copy
import re
import shutil

from check_inline import SECONDS
from check_orphans import digest
from linux_xattrs import INODE_INLINE_DATA, payload, returned_payload, snapshot

FILE = "/linux-inline"
DIRECTORY = "/linux-renamed-dir"
RETURN_DIRECTORY = "/core-return-dir"
RETURN_FILE = "/core-inline"


def logical(state):
    objects = copy.deepcopy(state["objects"])
    for item in objects.values():
        if item["inode"]["flags"] & INODE_INLINE_DATA:
            item["attrs"].pop("system.data", None)
    return objects


def check_objects(actual, wanted, changing, created):
    observed = logical(actual)
    if set(observed) != set(wanted):
        raise RuntimeError("Inline namespace differs, including orphan cleanup")
    old_numbers = {item["inode"]["inode"] for path, item in wanted.items() if path not in created}
    new_numbers = {observed[path]["inode"]["inode"] for path in created}
    if len(new_numbers) != len(created) or old_numbers & new_numbers:
        raise RuntimeError("Inline creation reused an allocated inode")
    for path, expected in wanted.items():
        item = observed[path]
        if item["attrs"] != expected["attrs"] or item.get("data") != expected.get("data"):
            raise RuntimeError(f"Inline bytes or attributes differ: {path}")
        for field, value in expected["inode"].items():
            if field not in changing.get(path, ()) and item["inode"].get(field) != value:
                raise RuntimeError(f"Inline inode field differs: {path} {field}")
        if "names" in expected:
            names = dict(expected["names"])
            for name, child in names.items():
                if isinstance(child, str):
                    names[name] = observed[child]["inode"]["inode"]
            if names != item["names"]:
                raise RuntimeError(f"Inline directory identities differ: {path}")
        if path in created and (not item["inode"]["flags"] & INODE_INLINE_DATA or
                                item["inode"]["blocks"] != 0):
            raise RuntimeError(f"New small inode is not inline: {path}")


def verify_linux(actual, before):
    wanted = logical(before)
    changed = wanted["/file120"]
    value = bytearray.fromhex(changed["data"])
    value[57:68] = payload(11)
    changed["data"] = value.hex()
    changed["attrs"]["user.linux"] = payload(13).hex()
    wanted[FILE] = dict(inode=dict(type="regular", mode=0o600, uid=0, gid=0, links=1, size=61),
                        data=payload(61).hex(), attrs={"user.small": payload(13).hex()})
    wanted[DIRECTORY] = dict(inode=dict(type="directory", mode=0o750, uid=0, gid=0, links=2),
                             names={".": DIRECTORY, "..": "/"}, attrs={})
    created = {FILE, DIRECTORY}
    for index in range(6):
        path = DIRECTORY + f"/n{index}"
        wanted[path] = dict(inode=dict(type="regular", mode=0o640, uid=0, gid=0, links=1, size=3),
                            data=payload(3).hex(), attrs={})
        wanted[DIRECTORY]["names"][f"n{index}"] = path
        created.add(path)
    wanted["/"]["names"].update({FILE[1:]: FILE, DIRECTORY[1:]: DIRECTORY})
    wanted["/"]["inode"]["links"] += 1
    check_objects(actual, wanted, {
        "/file120": {"ctime", "mtime", "blocks", "flags"},
        "/": {"ctime", "mtime", "size", "blocks", "flags"}}, created)
    if actual["accounting"]["Free inodes"] != before["accounting"]["Free inodes"] - len(created):
        raise RuntimeError("Linux inline creation/orphan accounting differs")


def verify_returned(actual, before):
    wanted = logical(before)
    changed = wanted[FILE]
    changed["data"] = (payload(1) + bytes(118) + b"\xd7").hex()
    changed["attrs"]["user.return"] = returned_payload(13).hex()
    changed["inode"].update(mode=0o640, uid=54321, gid=65432, size=120,
                             mtime=(SECONDS, 0), ctime=(SECONDS, 0))
    del wanted[DIRECTORY + "/n2"]
    del wanted[DIRECTORY]["names"]["n2"]
    wanted[DIRECTORY]["inode"].update(mtime=(SECONDS, 0), ctime=(SECONDS, 0))
    for path in list(wanted):
        if path == DIRECTORY or path.startswith(DIRECTORY + "/"):
            wanted[RETURN_DIRECTORY + path[len(DIRECTORY):]] = wanted.pop(path)
    root = wanted["/"]
    root["names"][RETURN_DIRECTORY[1:]] = root["names"].pop(DIRECTORY[1:])
    root["names"][RETURN_FILE[1:]] = RETURN_FILE
    root["inode"].update(mtime=(SECONDS, 0), ctime=(SECONDS, 0))
    wanted[RETURN_FILE] = dict(inode=dict(type="regular", mode=0o640, uid=70000, gid=80000,
                                         links=1, size=120, atime=(SECONDS, 0),
                                         mtime=(SECONDS, 0), ctime=(SECONDS, 0)),
                               attrs={}, data=(b"\xd7" * 120).hex())
    check_objects(actual, wanted, {FILE: {"blocks", "flags"},
                                  "/": {"size", "blocks", "flags"}}, {RETURN_FILE})
    if actual["accounting"]["Free inodes"] != before["accounting"]["Free inodes"]:
        raise RuntimeError("Core inline return inode accounting differs")


def verify(case, image, output, tools, recover, reader, run, native_replay):
    if native_replay is None:
        raise RuntimeError("Inline orphan recovery requires the native Linux reference")
    pending = output / f"linux-pending-{image.name}"
    oracle = output / f"linux-reference-{image.name}"
    shutil.copyfile(image, pending)
    pending_hash = digest(pending)
    shutil.copyfile(image, oracle)
    replay = run([recover, "--write", image])
    transactions = re.search(r"transactions=(\d+)", replay)
    orphans = re.search(r"orphans=(\d+)", replay)
    if transactions is None or int(transactions[1]) == 0:
        raise RuntimeError("Inline reverse roundtrip has no Linux transaction")
    if orphans is None or int(orphans[1]) == 0:
        raise RuntimeError("Inline reverse roundtrip did not clean the Linux orphan")
    states = []
    for label, candidate in (("core", image), ("oracle", oracle)):
        if label == "oracle":
            native_replay(candidate, image, "replay")
        state = snapshot(candidate, output / f"{label}-{image.stem}-checked", tools, run)
        verify_linux(state, case["xattr_before"])
        states.append(state)
    if states[0] != states[1]:
        raise RuntimeError("Core and native Linux inline recovery disagree")
    clean = digest(image)
    run([recover, "--write", image])
    if digest(image) != clean:
        raise RuntimeError("Repeated inline recovery changed the clean image")
    returned = output / f"returned-{image.name}"
    run([reader, "--linux-return", returned, image])
    after = snapshot(returned, output / f"returned-{image.stem}-checked", tools, run)
    verify_returned(after, states[0])
    if digest(pending) != pending_hash:
        raise RuntimeError("Linux-authored inline journal changed during checking")
    return dict(linux_authored_transactions=int(transactions[1]),
                linux_orphans_cleaned=int(orphans[1]), reverse_state=states[0],
                independent_replay="native Linux with strict nonrepairing e2fsck",
                linux_pending_image=str(pending), linux_pending_sha256=pending_hash,
                returned_image=str(returned), returned_sha256=digest(returned), returned_state=after)
