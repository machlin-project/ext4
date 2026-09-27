"""Independent expectations and reverse checks for the Linux namespace probe."""

import json
from pathlib import Path
import re
import shutil
import stat
import subprocess

from check_namespace import inode_fields, symlink_bytes
from check_orphans import accounting, digest


def node_name(index):
    return f"node-{index:08d}".ljust(255, "n")


def symlink_paths(case):
    name = Path(case["image"]).name
    if name.startswith("symlinks-"):
        return [f"/{prefix}{index}" for index in range(6)
                for prefix in ("symbolic-", "symbolic-alias-")]
    if re.match(r"(?:append-|group-)?atomic-[3-5]-", name):
        return ["/atomic-entry"]
    if name.startswith("basic-"):
        return ["/hello-link", "/symlink-alias"]
    return []


def prepare(case, tree, tools):
    image = Path(case["image"])
    if digest(image) != case["input_sha256"]:
        raise RuntimeError("Namespace oracle image changed after independent verification")
    commands = []

    def run(command):
        result = subprocess.run([str(x) for x in command], capture_output=True,
                                text=True, errors="backslashreplace", timeout=90)
        commands.append({"command": [str(x) for x in command], "status": result.returncode,
                         "stdout": result.stdout, "stderr": result.stderr})
        (tree.parent / f"{tree.name}-expectations.json").write_text(json.dumps(commands, indent=2) + "\n")
        result.check_returncode()
        return result.stdout

    exhaust = image.name.startswith("exhaust-")
    basic = image.name.startswith("basic-")
    paths = ["/", "/hello.txt"]
    data_paths = ["/hello.txt"]
    links = symlink_paths(case)
    link_lines = []
    expected = tree / "expected"
    expected.mkdir()
    if basic:
        paths += ["/created", "/created-dir", "/created-dir/child", "/created-dir/alias",
                  "/hello-link", "/symlink-alias"]
        data_paths += ["/created", "/created-dir/child", "/created-dir/alias"]
    elif exhaust:
        paths += [f"/{node_name(index)}" for index in range(case["inode_allocations"])]
        paths += ["/still-links"]
        data_paths += ["/still-links"]
    elif image.name.startswith("symlinks-"):
        paths += links
    else:
        paths += ["/atomic-entry"]
    (tree / "namespace-options").write_text(f"{int(exhaust)} {int(basic)}\n")
    inode_lines = []
    directories = []
    kinds = {"regular": stat.S_IFREG, "directory": stat.S_IFDIR, "symlink": stat.S_IFLNK}
    for path in paths:
        text = run([tools / "debugfs/debugfs", "-R", f"stat {path}", image])
        inode = inode_fields(text)
        kind = re.search(r"Type:\s+(\w+)", text)
        if inode is None or kind is None or kind[1] not in kinds:
            raise RuntimeError(f"Missing namespace inode or unsupported oracle type: {path}")
        values = [path, inode["inode"], f"{inode['mode'] | kinds[kind[1]]:o}", inode["uid"],
                  inode["gid"], inode["links"], inode["size"], inode["blocks"]]
        for field in ("atime", "mtime", "ctime"):
            seconds, extra = inode[field]
            seconds = seconds - (1 << 32) if seconds & (1 << 31) else seconds
            values += [seconds + ((extra & 3) << 32), extra >> 2]
        inode_lines.append(" ".join(str(value) for value in values))
        if kind[1] == "directory":
            listing = run([tools / "debugfs/debugfs", "-R", f"ls -p {path}", image])
            entries = []
            for line in listing.splitlines():
                if not line.strip():
                    continue
                fields = line.split("/")
                if len(fields) != 8 or not fields[5] or any(c.isspace() for c in fields[5]):
                    raise RuntimeError(f"Invalid namespace oracle listing: {line!r}")
                entries.append(f"{fields[5]} {int(fields[1])}")
            directories += [f"{path} {len(entries)}", *entries]
        elif kind[1] == "symlink":
            if path not in links:
                raise RuntimeError(f"Unexpected symbolic link in namespace oracle: {path}")
            target = symlink_bytes(image, path, inode, case["block_size"],
                                   tools / "debugfs/debugfs", run)
            name = f"link-{len(link_lines)}"
            (expected / name).write_bytes(target)
            link_lines.append(f"{path} /expected/{name}")
        elif path == "/atomic-entry":
            data_paths += [path]
    (tree / "namespace-inodes").write_text("\n".join(inode_lines) + "\n")
    (tree / "namespace-directories").write_text("\n".join(directories) + "\n")
    (tree / "namespace-links").write_text("\n".join(link_lines) + "\n")
    for index, path in enumerate(data_paths):
        run([tools / "debugfs/debugfs", "-R", f"dump {path} {expected / str(index)}", image])
        if not (expected / str(index)).is_file():
            raise RuntimeError(f"Missing independent contents for {path}")
    (tree / "namespace-files").write_text("\n".join(data_paths) + "\n")
    if digest(image) != case["input_sha256"]:
        raise RuntimeError("Read-only namespace expectation generation changed its input")


def verify(case, image, output, tools, recover, run):
    oracle = output / f"oracle-{image.name}"
    shutil.copyfile(image, oracle)
    replay = run([recover, "--write", image])
    transactions = re.search(r"transactions=(\d+)", replay)
    if transactions is None or int(transactions[1]) == 0:
        raise RuntimeError("Namespace roundtrip did not replay a Linux-authored transaction")
    # Replay only the journal and orphan cleanup. The following -fn performs no repairs.
    run([tools / "e2fsck/e2fsck", "-y", "-E", "journal_only", oracle], allowed=(0, 1))
    block_size = case["block_size"]
    expected = bytes(block_size + 3) + bytes((index * 13 + 0x6c) & 255 for index in range(73))

    def snapshot(candidate, prefix, allow_summary_lag=False):
        check = run([tools / "e2fsck/e2fsck", "-fn", candidate])
        header = run([tools / "misc/dumpe2fs", "-h", candidate])
        counts = accounting(header)
        groups = run([tools / "misc/dumpe2fs", candidate])
        totals = re.findall(r"^\s+(\d+) free blocks, (\d+) free inodes, \d+ directories", groups, re.M)
        if not totals or len(totals) != len(re.findall(r"^Group \d+:", groups, re.M)):
            raise RuntimeError("Missing independently counted block groups")
        lag = {}
        for index, field in enumerate(("Free blocks", "Free inodes")):
            total = sum(int(group[index]) for group in totals)
            if counts[field] != total:
                lag[field] = {"primary": counts[field], "group_total": total}
                counts[field] = total
        if lag and not allow_summary_lag:
            raise RuntimeError("Core recovery left stale primary accounting")
        # Linux journals group accounting while its primary totals can lag.
        # Journal-only replay deliberately does not repair those totals. Admit
        # only these exact, independently counted diagnostics in the oracle.
        diagnostics = {f"{field} count wrong ({value['primary']}, counted={value['group_total']})."
                       for field, value in lag.items()}
        observed_diagnostics = set()
        fix_pending = False
        for line in check.splitlines():
            if not line.strip():
                continue
            if line in diagnostics:
                observed_diagnostics.add(line)
                fix_pending = True
            elif line == "Fix? no" and fix_pending:
                fix_pending = False
            elif re.fullmatch(r"Pass [1-5]: .+", line) or re.fullmatch(
                    re.escape(str(candidate)) + r": \d+/\d+ files \([^\n]+\), \d+/\d+ blocks", line):
                continue
            else:
                raise RuntimeError(f"Unexpected nonrepairing e2fsck diagnostic: {line}")
        if observed_diagnostics != diagnostics or fix_pending:
            raise RuntimeError("Journal-only oracle accounting diagnostics disagree with group totals")
        inodes = {}
        for path in ("/", "/linux-dir", "/linux-dir/renamed", "/linux-link", "/linux-symlink"):
            inodes[path] = inode_fields(run([tools / "debugfs/debugfs", "-R", f"stat {path}", candidate]))
        retained_links = {}
        for path in symlink_paths(case):
            inode = inode_fields(run([tools / "debugfs/debugfs", "-R", f"stat {path}", candidate]))
            original = Path(case["image"])
            original_inode = inode_fields(run([tools / "debugfs/debugfs", "-R", f"stat {path}", original]))
            target = symlink_bytes(candidate, path, inode, block_size, tools / "debugfs/debugfs", run)
            expected_target = symlink_bytes(original, path, original_inode, block_size,
                                            tools / "debugfs/debugfs", run)
            if inode != original_inode or target != expected_target:
                raise RuntimeError("Linux roundtrip changed the core-created symbolic link")
            retained_links[path] = target.hex()
        file = inodes["/linux-dir/renamed"]
        if file is None or file != inodes["/linux-link"] or (
                file["uid"], file["gid"], file["mode"], file["links"], file["size"], file["blocks"]) != (
                    12345, 23456, 0o600, 2, len(expected), block_size // 512):
            raise RuntimeError("Linux-created file lost hardlink identity, allocation or attributes")
        data = output / f"{prefix}-{image.stem}.contents"
        run([tools / "debugfs/debugfs", "-R", f"dump /linux-dir/renamed {data}", candidate])
        if data.read_bytes() != expected:
            raise RuntimeError("Linux-created file has incorrect sparse or written bytes")
        link = run([tools / "debugfs/debugfs", "-R", "stat /linux-symlink", candidate])
        if 'Fast link dest: "linux-dir/renamed"' not in link:
            raise RuntimeError("Linux-created symlink was lost during replay")
        expected_free = case["accounting"]["Free inodes"]
        if not Path(case["image"]).name.startswith("exhaust-"):
            expected_free -= 3
        if counts["Free inodes"] != expected_free:
            raise RuntimeError("Linux inode reuse or primary free-inode reconstruction disagrees")
        names = {}
        for path in ("/", "/linux-dir"):
            names[path] = run([tools / "debugfs/debugfs", "-R", f"ls -p {path}", candidate])
        return {"inodes": inodes, "directories": names, "accounting": counts,
                "retained_symlinks": retained_links}, lag

    observed, _ = snapshot(image, "core")
    independent, lag = snapshot(oracle, "oracle", allow_summary_lag=True)
    if observed != independent:
        raise RuntimeError("Portable namespace recovery disagrees with e2fsprogs replay")
    stable = digest(image)
    run([recover, "--write", image])
    if digest(image) != stable:
        raise RuntimeError("Repeated recovery changed the Linux namespace result")
    return {"transactions": int(transactions[1]), "verified_namespace": observed,
            "oracle_primary_summary_lag": lag}
