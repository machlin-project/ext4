"""Indexed-directory expectations for the real Linux namespace roundtrip."""

import hashlib
from pathlib import Path

from check_index_write import entries
from check_namespace import inode_fields


def selected(case):
    return Path(case["image"]).name.startswith(("indexed-", "index-atomic-"))


def paths(case):
    name = Path(case["image"]).name
    result = ["/", "/hello.txt", "/indexed", "/lost+found"]
    if name.startswith("indexed-written-"):
        return result + ["/indexed/child", "/indexed/created", "/indexed/nested",
                         "/indexed/short-link", "/indexed/long-link", "/container"]
    result += ["/peer", "/peer/child"]
    if name.startswith("index-atomic-"):
        result += ["/indexed/child"]
    return result


def retained(case, image, output, tools, run, created, prefix):
    original = Path(case["image"])
    result = {}
    for path in paths(case):
        old = inode_fields(run([tools / "debugfs/debugfs", "-R", f"stat {path}", original]))
        new = inode_fields(run([tools / "debugfs/debugfs", "-R", f"stat {path}", image]))
        if old is None or new is None:
            raise RuntimeError(f"Missing retained indexed object {path}")
        expected = old
        if path == "/indexed":
            expected = dict(old, links=old["links"] + 1, size=new["size"], blocks=new["blocks"],
                            mtime=new["mtime"], ctime=new["ctime"])
            if new["size"] < old["size"] or new["blocks"] < old["blocks"]:
                raise RuntimeError("Linux unexpectedly shrank the indexed directory")
        if new != expected:
            raise RuntimeError(f"Linux changed retained indexed metadata {path}")
        item = dict(inode=new)
        if old["type"] == "directory":
            before = entries(run([tools / "debugfs/debugfs", "-R", f"ls -p {path}", original], raw=True))
            after = entries(run([tools / "debugfs/debugfs", "-R", f"ls -p {path}", image], raw=True))
            if path == "/indexed":
                before.update({name.encode(): created[f"/indexed/{name}"]["inode"]
                               for name in ("linux-dir", "linux-link", "linux-symlink")})
            if before != after:
                raise RuntimeError(f"Linux lost or changed an indexed byte name {path}")
            item["names"] = {name.hex(): number for name, number in after.items()}
        elif old["type"] == "regular":
            label = hashlib.sha256(path.encode()).hexdigest()[:16]
            dumps = []
            for kind, candidate in (("before", original), ("after", image)):
                dump = output / f"{prefix}-{image.stem}-{label}-{kind}.data"
                run([tools / "debugfs/debugfs", "-R", f"dump {path} {dump}", candidate])
                dumps.append(dump.read_bytes())
            if dumps[0] != dumps[1] or len(dumps[1]) != new["size"]:
                raise RuntimeError(f"Linux changed retained indexed contents {path}")
            item["data_sha256"] = hashlib.sha256(dumps[1]).hexdigest()
        result[path] = item
    # Collision exports also contain a distinct empty file reached by a non-UTF8
    # name. Linux checks its lookup through the complete directory oracle; verify
    # its full inode again after Linux has committed unrelated indexed entries.
    if case.get("other") is not None:
        number = case["other"]["inode"]
        before = inode_fields(run([tools / "debugfs/debugfs", "-R", f"stat <{number}>", original]))
        after = inode_fields(run([tools / "debugfs/debugfs", "-R", f"stat <{number}>", image]))
        if before is None or after != before:
            raise RuntimeError("Linux changed the retained distinct collision inode")
        result["collision_inode"] = after
    return result
