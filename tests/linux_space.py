"""Retained full-filesystem state in the real Linux namespace roundtrip."""

import hashlib
from pathlib import Path

from check_index_write import entries
from check_namespace import inode_fields


def selected(case):
    return case.get("verified_space") is not None


def paths(case):
    result = list(case["verified_space"]["objects"])
    if case["space_state"] == "reused":
        result += ["/room/renamed", "/room/short-link"]
    elif case["space_state"].endswith("-created"):
        result += [case["created_path"]]
    return result


def retained(case, image, output, tools, run, created, prefix, counts):
    original = Path(case["image"])
    result = {}
    released = 0
    index_blocks = 0
    block_size = case["block_size"]
    for path in paths(case):
        old = inode_fields(run([tools / "debugfs/debugfs", "-R", f"stat {path}", original]))
        new = inode_fields(run([tools / "debugfs/debugfs", "-R", f"stat {path}", image]))
        if old is None or new is None:
            raise RuntimeError(f"Missing retained full-space object {path}")
        expected = old
        if path == "/indexed":
            expected = dict(old, links=old["links"] + 1, size=new["size"], blocks=new["blocks"],
                            mtime=new["mtime"], ctime=new["ctime"])
            index_blocks = new["blocks"] - old["blocks"]
            if new["size"] < old["size"] or index_blocks < 0:
                raise RuntimeError("Linux unexpectedly shrank the full indexed directory")
        elif path == "/filler":
            expected = dict(old, size=0, blocks=0, mtime=new["mtime"], ctime=new["ctime"])
            released = old["blocks"]
        elif path == "/target":
            expected = dict(old, mtime=new["mtime"], ctime=new["ctime"])
        if new != expected:
            raise RuntimeError(f"Linux changed retained full-space metadata {path}")
        item = dict(inode=new)
        if old["type"] == "directory":
            before = entries(run([tools / "debugfs/debugfs", "-R", f"ls -p {path}", original], raw=True))
            after = entries(run([tools / "debugfs/debugfs", "-R", f"ls -p {path}", image], raw=True))
            if path == "/indexed":
                before.update({name.encode(): created[f"/indexed/{name}"]["inode"]
                               for name in ("linux-dir", "linux-link", "linux-symlink")})
            if before != after:
                raise RuntimeError(f"Linux lost or changed full-space names {path}")
            item["names"] = {name.hex(): number for name, number in after.items()}
        elif old["type"] == "regular":
            label = hashlib.sha256(path.encode()).hexdigest()[:16]
            contents = []
            for kind, candidate in (("before", original), ("after", image)):
                dump = output / f"{prefix}-{image.stem}-{label}-{kind}.data"
                run([tools / "debugfs/debugfs", "-R", f"dump {path} {dump}", candidate])
                contents.append(dump.read_bytes())
            expected_data = contents[0]
            if path == "/target":
                if len(expected_data) != block_size:
                    raise RuntimeError("Unexpected full-space overwrite target size")
                expected_data = expected_data[:-1] + b"Z"
            elif path == "/filler":
                expected_data = b""
            if contents[1] != expected_data or len(contents[1]) != new["size"]:
                raise RuntimeError(f"Linux changed retained full-space contents {path}")
            item["data_sha256"] = hashlib.sha256(contents[1]).hexdigest()
        result[path] = item
    allocated = sum(created[path]["blocks"] for path in ("/indexed/linux-dir", "/indexed/linux-link", "/indexed/linux-symlink"))
    sectors_per_block = block_size // 512
    if (released <= 0 or (released - index_blocks - allocated) % sectors_per_block != 0 or
            counts["Free blocks"] != (released - index_blocks - allocated) // sectors_per_block):
        raise RuntimeError("Linux full-disk release and reuse have incorrect block accounting")
    return result
