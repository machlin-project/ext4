"""Independent expectations and reverse checks for the Linux namespace probe."""

import hashlib
import json
from pathlib import Path
import re
import shutil
import stat
import subprocess

from check_namespace import inode_fields, symlink_bytes
from check_orphans import accounting, digest
from check_rename import entries
from check_index_write import entries as byte_entries
import linux_index
import linux_space


def node_name(index):
    return f"node-{index:08d}".ljust(255, "n")


def rename_objects(case):
    state = case.get("verified_rename")
    if state is None:
        return {}
    paths = {"root": "/", "hello": "/hello.txt", "left": "/left", "right": "/right",
             "source": "/left/source", "destination": case["destination_path"],
             "alias": "/kept-name", "filler": "/filler"}
    result = {}
    for key, path in paths.items():
        value = state.get(key)
        if value is not None:
            result[path] = value
            if "inside" in value:
                result[path + "/inside"] = value["inside"]
    return result


def symlink_paths(case):
    name = Path(case["image"]).name
    if linux_space.selected(case):
        return ["/room/short-link"] if case["space_state"] == "reused" else []
    if name.startswith("indexed-written-"):
        return ["/indexed/short-link", "/indexed/long-link"]
    if case.get("verified_rename"):
        return [path for path, value in rename_objects(case).items()
                if value["inode"]["type"] == "symlink"]
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

    def run(command, raw=False):
        result = subprocess.run([str(x) for x in command], capture_output=True,
                                timeout=90)
        commands.append({"command": [str(x) for x in command], "status": result.returncode,
                         "stdout": result.stdout.decode("utf-8", "backslashreplace"),
                         "stderr": result.stderr.decode("utf-8", "backslashreplace")})
        (tree.parent / f"{tree.name}-expectations.json").write_text(json.dumps(commands, indent=2) + "\n")
        result.check_returncode()
        return result.stdout if raw else result.stdout.decode("utf-8", "backslashreplace")

    exhaust = image.name.startswith("exhaust-")
    basic = image.name.startswith("basic-")
    paths = ["/", "/hello.txt"]
    data_paths = ["/hello.txt"]
    links = symlink_paths(case)
    renamed = rename_objects(case)
    full_blocks = linux_space.selected(case)
    indexed = linux_index.selected(case) or full_blocks
    link_lines = []
    expected = tree / "expected"
    expected.mkdir()
    if full_blocks:
        paths = linux_space.paths(case)
        data_paths = [path for path, item in case["verified_space"]["objects"].items()
                      if item["inode"]["type"] == "regular"]
        if case["space_state"] == "reused":
            data_paths += ["/room/renamed"]
    elif indexed:
        paths = linux_index.paths(case)
        if image.name.startswith("indexed-written-"):
            data_paths += ["/indexed/created"]
    elif renamed:
        paths = list(renamed)
        data_paths = [path for path, value in renamed.items() if value["inode"]["type"] == "regular"]
    elif basic:
        paths += ["/created", "/created-dir", "/created-dir/child", "/created-dir/alias",
                  "/hello-link", "/symlink-alias"]
        data_paths += ["/created", "/created-dir/child", "/created-dir/alias"]
    elif exhaust:
        paths += [f"/{node_name(index)}" for index in range(case["inode_allocations"])]
        paths += ["/still-links"]
        data_paths += ["/still-links"]
    elif image.name.startswith("symlinks-"):
        paths += links
    elif image.name.startswith(("removed-", "remove-atomic-")):
        if image.name.startswith("remove-atomic-0-"):
            paths += ["/kept-name"]
            data_paths += ["/kept-name"]
    else:
        paths += ["/atomic-entry"]
    (tree / "namespace-options").write_text(f"{int(exhaust)} {int(basic)} {int(indexed)} {int(full_blocks)}\n")
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
            listing = byte_entries(run([tools / "debugfs/debugfs", "-R", f"ls -p {path}", image], raw=True))
            encoded = [f"{name.hex()} {number}" for name, number in listing.items()]
            directories += [f"{path} {len(encoded)}", *encoded]
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
    full_blocks = linux_space.selected(case)
    indexed = linux_index.selected(case) or full_blocks
    base = "/indexed" if indexed else ""
    expected = bytes(block_size + 3) + bytes((index * 13 + 0x6c) & 255 for index in range(73))
    # e2fsck treats collapsing a valid extent tree as an optional optimization,
    # not a repair. Admit only the exact advice already present in a successful
    # nonrepairing check of this independently verified clean source image.
    source_advice = set()
    observed_advice = {}
    for command in case.get("commands", []):
        arguments = command["command"]
        if (command["status"] == 0 and "-fn" in arguments and
                Path(arguments[-1]).resolve() == Path(case["image"]).resolve()):
            source_advice.update(line for line in command.get("stdout", "").splitlines()
                                 if re.fullmatch(r"Inode \d+ extent tree \(at level \d+\) could be shorter\.  Optimize\? no", line))

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
        observed_advice[prefix] = []
        fix_pending = False
        for line in check.splitlines():
            if not line.strip():
                continue
            if line in diagnostics:
                observed_diagnostics.add(line)
                fix_pending = True
            elif line == "Fix? no" and fix_pending:
                fix_pending = False
            elif line in source_advice:
                observed_advice[prefix].append(line)
            elif re.fullmatch(r"Pass [1-5]: .+", line) or re.fullmatch(
                    re.escape(str(candidate)) + r": \d+/\d+ files \([^\n]+\), \d+/\d+ blocks", line):
                continue
            else:
                raise RuntimeError(f"Unexpected nonrepairing e2fsck diagnostic: {line}")
        if observed_diagnostics != diagnostics or fix_pending:
            raise RuntimeError("Journal-only oracle accounting diagnostics disagree with group totals")
        inodes = {}
        for path in (base + "/", base + "/linux-dir", base + "/linux-dir/renamed", base + "/linux-link", base + "/linux-symlink"):
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
        file = inodes[base + "/linux-dir/renamed"]
        if file is None or file != inodes[base + "/linux-link"] or (
                file["uid"], file["gid"], file["mode"], file["links"], file["size"], file["blocks"]) != (
                    12345, 23456, 0o600, 2, len(expected), block_size // 512):
            raise RuntimeError("Linux-created file lost hardlink identity, allocation or attributes")
        data = output / f"{prefix}-{image.stem}.contents"
        run([tools / "debugfs/debugfs", "-R", f"dump {base}/linux-dir/renamed {data}", candidate])
        if data.read_bytes() != expected:
            raise RuntimeError("Linux-created file has incorrect sparse or written bytes")
        link = run([tools / "debugfs/debugfs", "-R", f"stat {base}/linux-symlink", candidate])
        if 'Fast link dest: "linux-dir/renamed"' not in link:
            raise RuntimeError("Linux-created symlink was lost during replay")
        expected_free = case["accounting"]["Free inodes"]
        if not Path(case["image"]).name.startswith("exhaust-"):
            expected_free -= 3
        if counts["Free inodes"] != expected_free:
            raise RuntimeError("Linux inode reuse or primary free-inode reconstruction disagrees")
        names = {}
        for path in (base + "/", base + "/linux-dir"):
            names[path] = run([tools / "debugfs/debugfs", "-R", f"ls -p {path}", candidate])
        retained_alias = None
        retained_rename = {}
        original = Path(case["image"])
        for path, value in rename_objects(case).items():
            observed = inode_fields(run([tools / "debugfs/debugfs", "-R", f"stat {path}", candidate]))
            expected_inode = inode_fields(run([tools / "debugfs/debugfs", "-R", f"stat {path}", original]))
            if observed is None or (path != "/" and observed != expected_inode):
                raise RuntimeError(f"Linux changed a renamed inode's identity or attributes: {path}")
            retained = {"inode": observed}
            if value["inode"]["type"] == "directory":
                expected_names = dict(value["names"])
                if path == "/":
                    for name in ("linux-dir", "linux-link", "linux-symlink"):
                        expected_names[name] = inodes[f"/{name}"]["inode"]
                listing = entries(run([tools / "debugfs/debugfs", "-R", f"ls -p {path}", candidate]))
                if listing != expected_names:
                    raise RuntimeError(f"Linux changed rename topology or restored an old name: {path}")
                retained["names"] = listing
            else:
                if value["inode"]["type"] == "symlink":
                    contents = bytes.fromhex(retained_links[path])
                else:
                    label = hashlib.sha256(path.encode()).hexdigest()[:16]
                    dump = output / f"{prefix}-{image.stem}.rename-{label}.data"
                    run([tools / "debugfs/debugfs", "-R", f"dump {path} {dump}", candidate])
                    contents = dump.read_bytes()
                actual_hash = hashlib.sha256(contents).hexdigest()
                if len(contents) != value["data_size"] or actual_hash != value["data_sha256"]:
                    raise RuntimeError(f"Linux changed the contents of a renamed object: {path}")
                retained["data_sha256"] = actual_hash
            retained_rename[path] = retained
        if original.name.startswith(("removed-", "remove-atomic-")):
            expected_names = entries(run([tools / "debugfs/debugfs", "-R", "ls -p /", original]))
            if "victim" in expected_names:
                raise RuntimeError("Independently checked removal still contains its victim")
            for name in ("linux-dir", "linux-link", "linux-symlink"):
                expected_names[name] = inodes[f"/{name}"]["inode"]
            if entries(names["/"]) != expected_names:
                raise RuntimeError("Linux roundtrip lost an existing name or resurrected a removed name")
            if original.name.startswith("remove-atomic-0-"):
                retained_alias = inode_fields(run(
                    [tools / "debugfs/debugfs", "-R", "stat /kept-name", candidate]))
                original_alias = inode_fields(run(
                    [tools / "debugfs/debugfs", "-R", "stat /kept-name", original]))
                if retained_alias != original_alias:
                    raise RuntimeError("Linux changed the remaining hardlink's identity or attributes")
                original_data = output / f"{prefix}-{image.stem}.alias-before"
                retained_data = output / f"{prefix}-{image.stem}.alias-after"
                run([tools / "debugfs/debugfs", "-R", f"dump /kept-name {original_data}", original])
                run([tools / "debugfs/debugfs", "-R", f"dump /kept-name {retained_data}", candidate])
                if retained_data.read_bytes() != original_data.read_bytes():
                    raise RuntimeError("Linux changed bytes reachable through the remaining hardlink")
        retained_index = linux_index.retained(case, candidate, output, tools, run, inodes, prefix) if indexed and not full_blocks else {}
        retained_space = linux_space.retained(case, candidate, output, tools, run, inodes, prefix, counts) if full_blocks else {}
        return {"inodes": inodes, "directories": names, "accounting": counts,
                "retained_symlinks": retained_links, "retained_alias": retained_alias,
                "retained_rename": retained_rename, "retained_index": retained_index,
                "retained_space": retained_space}, lag

    observed, _ = snapshot(image, "core")
    independent, lag = snapshot(oracle, "oracle", allow_summary_lag=True)
    if observed != independent:
        raise RuntimeError("Portable namespace recovery disagrees with e2fsprogs replay")
    stable = digest(image)
    run([recover, "--write", image])
    if digest(image) != stable:
        raise RuntimeError("Repeated recovery changed the Linux namespace result")
    return {"transactions": int(transactions[1]), "verified_namespace": observed,
            "oracle_primary_summary_lag": lag,
            "source_extent_optimization_advice": sorted(source_advice),
            "observed_extent_optimization_advice": observed_advice}
