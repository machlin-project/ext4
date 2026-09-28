"""Linux expectations for distributed descriptors, retaining every seeded inode."""

import copy
import hashlib
import json

from check_geometry import snapshot, TARGET


def paths(case):
    objects = case["verified_geometry"]["objects"]
    empty = hashlib.sha256(b"").hexdigest()
    # The full root directory oracle covers every allocated name. Native data
    # reads cover the first populated inode in every group plus operation targets.
    return [path for path, value in objects.items()
            if path in ("/", "/hello.txt", "/empty", TARGET, "/geometry-extra") or
            value.get("data_sha256", empty) != empty]


def retained(case, image, output, tools, run, created, prefix):
    expected = copy.deepcopy(case["verified_geometry"])
    programs = {name: tools / directory / name for name, directory in
                (("debugfs", "debugfs"), ("dumpe2fs", "misc"), ("e2fsck", "e2fsck"))}
    # The namespace owner already checked fsck, including the narrowly verified
    # journal-only primary-summary lag. Retain all original inodes in one batch.
    observed = snapshot(image, output / f"{prefix}-{image.stem}-geometry", programs, run,
                        check=False, retained_paths=expected["objects"])
    observed = json.loads(json.dumps(observed))
    for name in ("linux-dir", "linux-link", "linux-symlink"):
        expected["names"][name] = created[f"/{name}"]["inode"]
    root = expected["objects"]["/"]
    new_root = observed["objects"]["/"]
    if (new_root["inode"]["size"] < root["inode"]["size"] or
            new_root["inode"]["blocks"] < root["inode"]["blocks"]):
        raise RuntimeError("Linux shrank the retained geometry directory")
    root["inode"].update(links=root["inode"]["links"] + 1,
                         size=new_root["inode"]["size"], blocks=new_root["inode"]["blocks"],
                         mtime=new_root["inode"]["mtime"], ctime=new_root["inode"]["ctime"])
    root["mapping"] = new_root["mapping"]
    if case["geometry_operation"] == "write":
        size = case["block_size"]
        data = bytearray(size + 7) + bytearray((index * 31 + 7) & 255 for index in range(size + 3))
        item = expected["objects"][TARGET]
        if item["data_sha256"] != hashlib.sha256(data).hexdigest():
            raise RuntimeError("Geometry write input differs from accepted pattern")
        data[size + 7] ^= 0x5a
        item["data_sha256"] = hashlib.sha256(data).hexdigest()
        actual = observed["objects"][TARGET]["inode"]
        item["inode"].update(mtime=actual["mtime"], ctime=actual["ctime"])
    # The parent snapshot separately validates Linux's new inode/data accounting.
    # Return the retained state without the unnormalized primary free summary.
    del observed["accounting"]
    del expected["accounting"]
    if observed != expected:
        raise RuntimeError("Linux changed retained geometry data, metadata or descriptor locations")
    return observed
