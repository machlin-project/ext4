"""Verify native Linux cluster allocation, orphan replay and core return."""

import re
import shutil

from check_clusters import SECONDS
from check_orphans import digest
from linux_inline import logical
from linux_xattrs import SECTOR_BYTES, payload, returned_payload, snapshot

FILE = "/linux-cluster"
DIRECTORY = "/linux-cluster-dir"
RETURN_FILE = "/core-cluster"

def check_objects(actual, wanted, changing, created):
    observed = logical(actual)
    if set(observed) != set(wanted):
        raise RuntimeError("Cluster namespace differs, including orphan cleanup")
    old_numbers = {item["inode"]["inode"] for path, item in wanted.items() if path not in created}
    new_numbers = {observed[path]["inode"]["inode"] for path in created}
    if len(new_numbers) != len(created) or old_numbers & new_numbers:
        raise RuntimeError("Cluster creation reused an allocated inode")
    for path, expected in wanted.items():
        item = observed[path]
        if item["attrs"] != expected["attrs"] or item.get("data") != expected.get("data"):
            raise RuntimeError(f"Cluster bytes or attributes differ: {path}")
        for field, value in expected["inode"].items():
            if field not in changing.get(path, ()) and item["inode"].get(field) != value:
                raise RuntimeError(f"Cluster inode field differs: {path} {field}")
        if "names" in expected:
            names = dict(expected["names"])
            for name, child in names.items():
                if isinstance(child, str):
                    names[name] = observed[child]["inode"]["inode"]
            if names != item["names"]:
                raise RuntimeError(f"Cluster directory identities differ: {path}")

def verify_accounting(actual, before, inode_delta):
    if actual["accounting"]["Free inodes"] != before["accounting"]["Free inodes"] + inode_delta:
        raise RuntimeError("Cluster creation/orphan inode accounting differs")
    block = before["accounting"]["Block size"]
    # These fixtures have unique inode paths and no shared attribute blocks.
    sectors = sum(item["inode"]["blocks"] for item in actual["objects"].values())
    sectors -= sum(item["inode"]["blocks"] for item in before["objects"].values())
    if sectors * SECTOR_BYTES % block or actual["accounting"]["Free blocks"] != before["accounting"]["Free blocks"] - sectors * SECTOR_BYTES // block:
        raise RuntimeError("Cluster physical and inode block charges disagree")


def verify_linux(actual, before, ratio):
    wanted = logical(before)
    block = before["accounting"]["Block size"]
    cluster = block * ratio
    changed = wanted["/sparse"]
    value = bytearray.fromhex(changed["data"])
    value[3 * cluster:4 * cluster] = bytes(cluster)
    value[2 * cluster + 2 * block:2 * cluster + 3 * block] = payload(block)
    changed["data"] = value.hex()
    wanted[FILE] = dict(inode=dict(type="regular", mode=0o600, uid=0, gid=0, links=1,
                                   size=2 * cluster + block, blocks=5 * cluster // SECTOR_BYTES),
                        data=payload(2 * cluster + block).hex(), attrs={"user.large": payload(block // 2).hex()})
    wanted[DIRECTORY] = dict(inode=dict(type="directory", mode=0o750, uid=0, gid=0, links=2),
                             names={".": DIRECTORY, "..": "/", "child": DIRECTORY + "/child"}, attrs={})
    wanted[DIRECTORY + "/child"] = dict(inode=dict(type="regular", mode=0o640, uid=0, gid=0, links=1, size=61),
                                        data=payload(61).hex(), attrs={})
    wanted["/"]["names"].update({FILE[1:]: FILE, DIRECTORY[1:]: DIRECTORY})
    wanted["/"]["inode"]["links"] += 1
    check_objects(actual, wanted, {"/sparse": {"ctime", "mtime", "blocks", "flags"},
                                    "/": {"ctime", "mtime", "size", "blocks", "flags"}},
                  {FILE, DIRECTORY, DIRECTORY + "/child"})
    verify_accounting(actual, before, -3)


def verify_returned(actual, before, ratio):
    wanted = logical(before)
    block = before["accounting"]["Block size"]
    cluster = block * ratio
    changed = wanted[FILE]
    value = bytearray(cluster + 20)
    value[block:block + 5] = payload(block + 5)[block:]
    value[cluster + 3:] = b"\xd7" * 17
    changed["data"] = value.hex()
    changed["attrs"] = {"user.return": returned_payload(13).hex()}
    changed["inode"].update(mode=0o640, uid=54321, gid=65432, size=len(value),
                             blocks=2 * cluster // SECTOR_BYTES,
                             mtime=(SECONDS, 0), ctime=(SECONDS, 0))
    del wanted[DIRECTORY + "/child"]
    del wanted[DIRECTORY]
    root = wanted["/"]
    del root["names"][DIRECTORY[1:]]
    root["names"][RETURN_FILE[1:]] = RETURN_FILE
    root["inode"].update(links=root["inode"]["links"] - 1, mtime=(SECONDS, 0), ctime=(SECONDS, 0))
    wanted[RETURN_FILE] = dict(inode=dict(type="regular", mode=0o640, uid=70000, gid=80000,
                                         links=1, size=cluster + 7, blocks=2 * cluster // SECTOR_BYTES,
                                         atime=(SECONDS, 0), mtime=(SECONDS, 0), ctime=(SECONDS, 0)),
                               attrs={}, data=(b"\xd7" * (cluster + 7)).hex())
    check_objects(actual, wanted, {"/": {"size", "blocks", "flags"}}, {RETURN_FILE})
    verify_accounting(actual, before, 1)


def verify(case, image, output, tools, recover, reader, run, native_replay):
    if native_replay is None:
        raise RuntimeError("Cluster orphan recovery requires the native Linux reference")
    pending = output / f"linux-pending-{image.name}"
    oracle = output / f"linux-reference-{image.name}"
    shutil.copyfile(image, pending)
    pending_hash = digest(pending)
    shutil.copyfile(image, oracle)
    replay = run([recover, "--write", image])
    transactions = re.search(r"transactions=(\d+)", replay)
    orphans = re.search(r"orphans=(\d+)", replay)
    if transactions is None or int(transactions[1]) == 0:
        raise RuntimeError("Cluster reverse roundtrip has no Linux transaction")
    if orphans is None or int(orphans[1]) == 0:
        raise RuntimeError("Cluster reverse roundtrip did not clean the Linux orphan")
    states = []
    for label, candidate in (("core", image), ("oracle", oracle)):
        if label == "oracle":
            native_replay(candidate, image, "replay")
        state = snapshot(candidate, output / f"{label}-{image.stem}-checked", tools, run)
        verify_linux(state, case["xattr_before"], case["cluster_blocks"])
        states.append(state)
    if states[0] != states[1]:
        raise RuntimeError("Core and native Linux cluster recovery disagree")
    clean = digest(image)
    run([recover, "--write", image])
    if digest(image) != clean:
        raise RuntimeError("Repeated cluster recovery changed the clean image")
    returned = output / f"returned-{image.name}"
    run([reader, "--linux-return", returned, image])
    after = snapshot(returned, output / f"returned-{image.stem}-checked", tools, run)
    verify_returned(after, states[0], case["cluster_blocks"])
    if digest(pending) != pending_hash:
        raise RuntimeError("Linux-authored cluster journal changed during checking")
    return dict(linux_authored_transactions=int(transactions[1]),
                linux_orphans_cleaned=int(orphans[1]), reverse_state=states[0],
                independent_replay="native Linux with strict nonrepairing e2fsck",
                linux_pending_image=str(pending), linux_pending_sha256=pending_hash,
                returned_image=str(returned), returned_sha256=digest(returned), returned_state=after)
