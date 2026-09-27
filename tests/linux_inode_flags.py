"""Verify Linux policy-bit writes and their portable journal recovery."""

import copy
import json
from pathlib import Path
import re
import shutil

from check_inode_flags import IMMUTABLE, APPEND, NODUMP, NOATIME, DIRSYNC, EXTENTS, POLICY
from check_orphans import digest
from linux_xattrs import snapshot


def expected(before, actual):
    result = copy.deepcopy(before)
    objects = result["objects"]
    source = objects["/block"]
    inode = source["inode"]
    times = actual["objects"]["/block"]["inode"]
    inode.update(flags=(inode["flags"] & ~POLICY) | IMMUTABLE | NODUMP | NOATIME,
                 size=inode["size"] + 1, mtime=times["mtime"], ctime=times["ctime"])
    source["data"] += b"Z".hex()
    block_size = before["accounting"]["Block size"]
    sectors = block_size // 512
    growth_blocks = int(inode["size"] == 1)
    inode["blocks"] += growth_blocks * sectors
    root = objects["/"]
    root_times = actual["objects"]["/"]["inode"]
    root["inode"].update(links=root["inode"]["links"] + 1,
                         mtime=root_times["mtime"], ctime=root_times["ctime"])
    mapping = root["inode"]["flags"] & EXTENTS
    existing = {obj["inode"]["inode"] for obj in objects.values()}
    identities = {}
    for path, kind, mode, size, links, flags in (
            ("/linux-flag-dir", "directory", 0o700, block_size, 2,
             mapping | APPEND | NODUMP | NOATIME | DIRSYNC),
            ("/linux-flag-dir/child", "regular", 0o600, 1, 1,
             mapping | NODUMP | NOATIME)):
        found = actual["objects"].get(path)
        if found is None or found["inode"]["inode"] in existing:
            raise RuntimeError("Missing or aliased Linux flag inode")
        node = found["inode"]
        existing.add(node["inode"])
        identities[path] = node["inode"]
        wanted = {key: node[key] for key in ("inode", "generation", "atime", "mtime", "ctime", "crtime")}
        wanted.update(type=kind, mode=mode, uid=0, gid=0, size=size, blocks=sectors,
                      links=links, flags=flags)
        objects[path] = dict(inode=wanted, attrs={})
    root["names"]["linux-flag-dir"] = identities["/linux-flag-dir"]
    objects["/linux-flag-dir"]["names"] = {
        ".": identities["/linux-flag-dir"], "..": root["inode"]["inode"],
        "child": identities["/linux-flag-dir/child"]}
    objects["/linux-flag-dir/child"]["data"] = b"L".hex()
    result["accounting"]["Free inodes"] -= 2
    result["accounting"]["Free blocks"] -= 2 + growth_blocks
    return result


def verify(case, image, output, tools, recover, run):
    oracle = output / f"oracle-{image.name}"
    shutil.copyfile(image, oracle)
    replay = run([recover, "--write", image])
    transactions = re.search(r"transactions=(\d+)", replay)
    if transactions is None or int(transactions[1]) == 0:
        raise RuntimeError("Flag roundtrip did not replay a Linux journal")
    run([tools / "e2fsck/e2fsck", "-y", "-E", "journal_only", oracle])
    before = case["verified_flags"]
    states = []
    lag = None
    for engine, candidate in (("core", image), ("oracle", oracle)):
        current = snapshot(candidate, output / f"{engine}-{image.stem}-flags", tools, run,
                           allow_summary_lag=engine == "oracle")
        current = json.loads(json.dumps(current))
        if engine == "oracle":
            lag = current["oracle_summary_lag"]
            current["oracle_summary_lag"] = {}
        if current != expected(before, current):
            raise RuntimeError(f"{engine} lost Linux flag policy, exact data or retained attributes")
        states.append(current)
    if states[0] != states[1]:
        raise RuntimeError("Core and oracle flag recovery disagree")
    stable = digest(image)
    run([recover, "--write", image])
    if digest(image) != stable:
        raise RuntimeError("Repeated flag recovery changes the resource")
    return dict(transactions=int(transactions[1]), verified_flags=states[0],
                oracle_primary_summary_lag=lag)
