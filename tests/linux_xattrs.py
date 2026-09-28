"""Independent attribute expectations for Linux and portable-core roundtrips."""

import copy
import json
from pathlib import Path
import re
import shutil
import stat
import struct
import subprocess

from check_namespace import inode_fields, symlink_bytes, INODE_BLOCK_DATA_SIZE
from check_orphans import accounting, digest
from check_rename import entries
from check_xattrs import PREFIXES, listed_name
from generate_xattr_fixtures import (ACL_VERSION, ACL_USER_OBJ, ACL_USER, ACL_GROUP_OBJ,
                                     ACL_GROUP, ACL_MASK, ACL_OTHER)

ACL_USERSPACE_VERSION = 2
ACL_UNDEFINED_ID = 0xffffffff
CAPABILITY_REVISION_2 = 0x02000000
CAPABILITY_EFFECTIVE = 1
CAP_NET_BIND_SERVICE = 10
SECTOR_BYTES = 512
INODE_INLINE_DATA = 0x10000000
FILE = "/linux-xattr-file"
DIRECTORY = "/linux-xattr-directory"
INHERITED = "/directory/linux-inherited"
RETURN_SECONDS = 1700000090
LINUX_ACL_ENTRIES = ((ACL_USER_OBJ, 6, None), (ACL_USER, 6, 70000),
                     (ACL_GROUP_OBJ, 4, None), (ACL_GROUP, 4, 80000),
                     (ACL_MASK, 4, None), (ACL_OTHER, 0, None))


def payload(size):
    return bytes((index * 23 + 0x67) & 255 for index in range(size))


def returned_payload(size):
    return bytes((index * 31 + 0x49) & 255 for index in range(size))


def acl_bytes(items, userspace=False):
    result = struct.pack("<I", ACL_USERSPACE_VERSION if userspace else ACL_VERSION)
    for tag, permissions, identity in items:
        result += struct.pack("<HH", tag, permissions)
        if userspace or identity is not None:
            result += struct.pack("<I", ACL_UNDEFINED_ID if identity is None else identity)
    return result


def acl_entries(value):
    if len(value) < 4 or struct.unpack_from("<I", value)[0] != ACL_VERSION:
        raise RuntimeError("Expected compact ext4 ACL version 1")
    result = []
    offset = 4
    while offset < len(value):
        tag, permissions = struct.unpack_from("<HH", value, offset)
        offset += 4
        if tag not in (ACL_USER_OBJ, ACL_USER, ACL_GROUP_OBJ, ACL_GROUP, ACL_MASK, ACL_OTHER):
            raise RuntimeError("Unknown ACL tag in the independent fixture")
        identity = None
        if tag in (ACL_USER, ACL_GROUP):
            identity = struct.unpack_from("<I", value, offset)[0]
            offset += 4
        result.append((tag, permissions, identity))
    if offset != len(value):
        raise RuntimeError("Truncated compact ACL")
    return result


def acl_mode(value, mode):
    changes = {ACL_USER_OBJ: (mode >> 6) & 7, ACL_MASK: (mode >> 3) & 7,
               ACL_OTHER: mode & 7}
    return acl_bytes([(tag, changes.get(tag, permissions), identity)
                      for tag, permissions, identity in acl_entries(value)])


def namespace(name):
    for index in (2, 3, 8, 1, 4, 6, 7):
        prefix = PREFIXES[index]
        if name.startswith(prefix):
            suffix = name[len(prefix):]
            if index in (2, 3, 8) and suffix:
                continue
            return index, suffix.encode()
    raise RuntimeError(f"Unsupported attribute namespace in this Linux profile: {name!r}")


def tool(root, name):
    if isinstance(root, dict):
        return root[name]
    return root / ("misc" if name == "dumpe2fs" else name) / name


def snapshot(image, output, tools, run, *, allow_summary_lag=False):
    output.mkdir(parents=True, exist_ok=False)
    check = run([tool(tools, "e2fsck"), "-fn", image],
                allowed=(0, 4) if allow_summary_lag else (0,))
    header = run([tool(tools, "dumpe2fs"), "-h", image])
    counts = accounting(header)
    cluster = re.search(r"^Cluster size:\s+(\d+)$", header, re.M)
    cluster_blocks = int(cluster[1]) // counts["Block size"] if cluster else 1
    ea_inode = re.search(r"^Filesystem features:.*\bea_inode\b", header, re.M) is not None
    groups = run([tool(tools, "dumpe2fs"), image])
    totals = re.findall(r"^\s+(\d+) free (blocks|clusters), (\d+) free inodes, \d+ directories", groups, re.M)
    if not totals or len(totals) != len(re.findall(r"^Group \d+:", groups, re.M)):
        raise RuntimeError("Missing independent group accounting")
    lag = {}
    for field in ("Free blocks", "Free inodes"):
        total = sum(int(group[0]) * (cluster_blocks if group[1] == "clusters" else 1)
                    if field == "Free blocks" else int(group[2]) for group in totals)
        if counts[field] != total:
            lag[field] = dict(primary=counts[field], group_total=total)
            counts[field] = total
    if lag and not allow_summary_lag:
        raise RuntimeError("Stale primary allocation totals")
    # Only the independently counted Linux primary-summary lag is admissible.
    diagnostics = {f"{name} count wrong ({value['primary']}, counted={value['group_total']})."
                   for name, value in lag.items()}
    observed = set()
    question = False
    for line in check.splitlines():
        if not line.strip():
            continue
        if line in diagnostics:
            observed.add(line)
            question = True
        elif line == "Fix? no" and question:
            question = False
        elif re.fullmatch(r"Pass [1-5]: .+", line) or re.fullmatch(
                re.escape(str(image)) + r": \d+/\d+ files \([^\n]+\), \d+/\d+ blocks", line):
            continue
        else:
            raise RuntimeError(f"Unexpected nonrepairing e2fsck diagnostic: {line}")
    if observed != diagnostics or question:
        raise RuntimeError("Oracle accounting diagnostics disagree with group totals")
    objects = {}
    pending = ["/"]
    while pending:
        path = pending.pop(0)
        if path in objects or len(objects) >= 128 or any(c.isspace() for c in path):
            raise RuntimeError("Unexpected cyclic, large or whitespace-bearing fixture namespace")
        raw = run([tool(tools, "debugfs"), "-R", f"stat {path}", image])
        inode = inode_fields(raw)
        if inode is None:
            raise RuntimeError(f"Missing independently decoded inode: {path}")
        item = objects[path] = dict(inode=inode, attrs={})
        listing = run([tool(tools, "debugfs"), "-R", f"ea_list {path}", image])
        names = re.findall(r"^  (.+?) \((\d+)\)(?: =.*)?$", listing, re.M)
        for index, (encoded, length) in enumerate(names):
            name = listed_name(encoded)
            namespace(name)
            if name in item["attrs"] or '"' in name or "\\" in name:
                raise RuntimeError("Duplicate or unrepresentable attribute oracle key")
            value = output / f"attribute-{len(objects)}-{index}"
            run([tool(tools, "debugfs"), "-R", f'ea_get -r -f "{value}" {path} "{name}"', image])
            if not value.is_file() or value.stat().st_size != int(length):
                raise RuntimeError("Independent attribute size differs")
            item["attrs"][name] = value.read_bytes().hex()
        if inode["type"] == "directory":
            item["names"] = entries(run([tool(tools, "debugfs"), "-R", f"ls -p {path}", image]))
            pending += [path.rstrip("/") + "/" + name for name in item["names"] if name not in (".", "..")]
        elif inode["type"] == "symlink":
            external = re.search(r"^File ACL:\s+(\d+)", raw, re.M)
            if external is None:
                raise RuntimeError("Missing independent attribute block pointer")
            data_inode = dict(inode)
            data_inode["blocks"] -= int(int(external[1]) != 0) * cluster_blocks * counts["Block size"] // SECTOR_BYTES
            if ea_inode:
                # The preceding strict fsck validates total data plus logical
                # value charges. Resolve the symlink target independently via
                # bmap below; its data-only accounting excludes private values.
                data_inode["blocks"] = (int(inode["size"] >= INODE_BLOCK_DATA_SIZE) *
                                        cluster_blocks * counts["Block size"] // SECTOR_BYTES)
            item["data"] = symlink_bytes(image, path, data_inode, counts["Block size"],
                                         tool(tools, "debugfs"), run,
                                         cluster_blocks=cluster_blocks).hex()
        elif inode["type"] == "regular":
            value = output / f"data-{len(objects)}"
            run([tool(tools, "debugfs"), "-R", f'dump {path} "{value}"', image])
            if not value.is_file():
                raise RuntimeError("Missing independently extracted file")
            raw = value.read_bytes()
            if inode["flags"] & INODE_INLINE_DATA:
                # libext2fs returns inode storage capacity, including bytes
                # beyond logical EOF; those bytes are not visible file data.
                if len(raw) < inode["size"]:
                    raise RuntimeError("Truncated independent inline storage")
                raw = raw[:inode["size"]]
            if len(raw) != inode["size"]:
                raise RuntimeError("Independent file length differs")
            item["data"] = raw.hex()
        elif inode["type"] not in ("character", "block", "FIFO", "socket"):
            raise RuntimeError("Unsupported inode type in the attribute roundtrip fixture")
    return dict(objects=objects, accounting=counts, oracle_summary_lag=lag)


def prepare(case, tree, tools, *, verify_only=False):
    image = Path(case["image"])
    if digest(image) != case["input_sha256"]:
        raise RuntimeError("Attribute source changed before Linux preparation")
    commands = []

    def run(command, allowed=(0,)):
        done = subprocess.run([str(p) for p in command], capture_output=True,
                              text=True, errors="backslashreplace", timeout=120)
        commands.append(dict(command=[str(p) for p in command], status=done.returncode,
                             stdout=done.stdout, stderr=done.stderr))
        (tree.parent / f"{tree.name}-expectations.json").write_text(json.dumps(commands, indent=2) + "\n")
        if done.returncode not in allowed:
            raise RuntimeError(f"Independent attribute preparation failed: {command}")
        return done.stdout

    state = snapshot(image, tree.parent / f"{tree.name}-snapshot", tools, run)
    case["xattr_before"] = state
    expected = tree / "expected"
    expected.mkdir()
    kinds = dict(regular=stat.S_IFREG, directory=stat.S_IFDIR, symlink=stat.S_IFLNK)
    inode_lines, attribute_lines, data_lines = [], [], []
    for path, item in state["objects"].items():
        inode = item["inode"]
        visible_attrs = {name: value for name, value in item["attrs"].items()
                         if name != "system.data" or not inode["flags"] & INODE_INLINE_DATA}
        inode_lines.append(f"{path} {inode['inode']} {inode['mode'] | kinds[inode['type']]:o} "
                           f"{inode['uid']} {inode['gid']} {inode['size']} {len(visible_attrs)}")
        for name, value in visible_attrs.items():
            index, _ = namespace(name)
            raw = bytes.fromhex(value)
            if index in (2, 3):
                raw = acl_bytes(acl_entries(raw), userspace=True)
            filename = f"attribute-{len(attribute_lines)}"
            (expected / filename).write_bytes(raw)
            denied = int(inode["type"] == "symlink" and index == 1)
            attribute_lines.append(f"{path} {name.encode().hex()} /expected/{filename} {denied}")
        if "data" in item:
            filename = f"data-{len(data_lines)}"
            (expected / filename).write_bytes(bytes.fromhex(item["data"]))
            data_lines.append(f"{path} /expected/{filename} {int(inode['type'] == 'symlink')}")
    for filename, lines in (("xattr-inodes", inode_lines), ("xattr-values", attribute_lines),
                            ("xattr-data", data_lines)):
        (tree / filename).write_text("\n".join(lines) + "\n")
    (tree / "xattr-options").write_text(f"{int(verify_only)}\n")
    if case.get("ea_inode"):
        (tree / "ea-inode").touch()
    if case.get("inline_data"):
        (tree / "inline-data").touch()
    (tree / "linux-acl").write_bytes(acl_bytes(LINUX_ACL_ENTRIES, userspace=True))
    (tree / "linux-capability").write_bytes(struct.pack(
        "<IIIII", CAPABILITY_REVISION_2 | CAPABILITY_EFFECTIVE, 1 << CAP_NET_BIND_SERVICE, 0, 0, 0))
    if digest(image) != case["input_sha256"]:
        raise RuntimeError("Read-only attribute expectation generation changed its source")


def expected_linux(before):
    objects = copy.deepcopy(before["objects"])
    objects["/block"]["attrs"]["user.binary"] = payload(650).hex()
    objects["/block"]["inode"]["blocks"] = before["accounting"]["Block size"] // SECTOR_BYTES
    objects["/shared"]["attrs"].pop("user.binary", None)
    objects["/shared"]["inode"]["blocks"] = 0
    acl = bytes.fromhex(objects["/many"]["attrs"]["system.posix_acl_access"])
    objects["/many"]["attrs"]["system.posix_acl_access"] = acl_mode(acl, 0o640).hex()
    objects["/many"]["inode"]["mode"] = 0o640
    capability = struct.pack("<IIIII", CAPABILITY_REVISION_2 | CAPABILITY_EFFECTIVE,
                             1 << CAP_NET_BIND_SERVICE, 0, 0, 0)
    objects[FILE] = dict(inode=dict(type="regular", mode=0o640, uid=12345, gid=23456,
                                    links=1, size=17,
                                    blocks=2 * before["accounting"]["Block size"] // SECTOR_BYTES),
                         data=payload(17).hex(), attrs={
        "user.binary": payload(600).hex(), "user.empty": "", "trusted.marker": payload(19).hex(),
        "system.posix_acl_access": acl_bytes(LINUX_ACL_ENTRIES).hex(),
        "security.capability": capability.hex()})
    objects[DIRECTORY] = dict(inode=dict(type="directory", mode=0o750, uid=0, gid=0, links=2),
                              attrs={"system.posix_acl_default": acl_bytes(LINUX_ACL_ENTRIES).hex()})
    inherited = bytes.fromhex(objects["/directory"]["attrs"]["system.posix_acl_default"])
    objects[INHERITED] = dict(inode=dict(type="regular", mode=0o640, uid=0, gid=0, links=1, size=0),
                              data="", attrs={"system.posix_acl_access": acl_mode(inherited, 0o640).hex()})
    objects["/"]["inode"]["links"] += 1
    return objects


def verify_expected(actual, expected, *, newly_created=True):
    if set(actual["objects"]) != set(expected):
        raise RuntimeError("Linux attribute roundtrip changed the inode namespace")
    new_paths = {FILE, DIRECTORY, INHERITED} if newly_created else set()
    changing = {"/block": {"ctime"}, "/shared": {"ctime"},
                "/many": {"ctime"}, "/": {"ctime", "mtime", "size", "blocks"},
                "/directory": {"ctime", "mtime", "size", "blocks"}}
    for path, wanted in expected.items():
        item = actual["objects"][path]
        if item["attrs"] != wanted["attrs"] or item.get("data") != wanted.get("data"):
            raise RuntimeError(f"Attribute names, bytes or file data differ: {path}")
        ignored = changing.get(path, set()) if newly_created else set()
        for field, value in wanted["inode"].items():
            if field not in ignored and item["inode"].get(field) != value:
                raise RuntimeError(f"Inode field differs after attribute roundtrip: {path} {field}")
        if "names" in wanted:
            names = dict(wanted["names"])
            if newly_created and path in ("/", "/directory"):
                for child in new_paths:
                    if str(Path(child).parent) == path:
                        names[Path(child).name] = actual["objects"][child]["inode"]["inode"]
            if names != item["names"]:
                raise RuntimeError(f"Directory identity differs: {path}")
    if newly_created:
        old_numbers = {item["inode"]["inode"] for path, item in expected.items() if path not in new_paths}
        new_numbers = {actual["objects"][path]["inode"]["inode"] for path in new_paths}
        if len(new_numbers) != len(new_paths) or old_numbers & new_numbers:
            raise RuntimeError("Linux reused an allocated inode identity")
        directory = actual["objects"][DIRECTORY]
        if directory["names"] != {".": directory["inode"]["inode"],
                                  "..": actual["objects"]["/"]["inode"]["inode"]}:
            raise RuntimeError("Linux-created directory has incorrect dot entries")


def write_core_expectations(state, output):
    output.mkdir(parents=True, exist_ok=False)
    lines = []
    for path, item in state["objects"].items():
        if not item["attrs"]:
            lines.append(f"{path} -1 - -")
        for name, value in item["attrs"].items():
            index, suffix = namespace(name)
            payload_name = f"value-{len(lines)}"
            (output / payload_name).write_bytes(bytes.fromhex(value))
            lines.append(f"{path} {index} {suffix.hex() or '-'} {payload_name}")
    manifest = output / "values.expected"
    manifest.write_text("\n".join(lines) + "\n")
    return manifest


def verify(case, image, output, tools, recover, reader, run, *, native_replay=None):
    if case.get("inline_data"):
        from linux_inline import verify as verify_inline
        return verify_inline(case, image, output, tools, recover, reader, run, native_replay)
    if case.get("ea_inode"):
        from linux_ea_inode import verify as verify_ea
        return verify_ea(case, image, output, tools, recover, reader, run, native_replay)
    oracle = output / f"oracle-{image.name}"
    shutil.copyfile(image, oracle)
    replay = run([recover, "--write", image])
    transactions = re.search(r"transactions=(\d+)", replay)
    if transactions is None or int(transactions[1]) == 0:
        raise RuntimeError("Attribute roundtrip did not replay a Linux transaction")
    run([tool(tools, "e2fsck"), "-y", "-E", "journal_only", oracle], allowed=(0, 1))
    states = []
    for label, candidate in (("core", image), ("oracle", oracle)):
        state = snapshot(candidate, output / f"{label}-{image.stem}-checked", tools, run,
                         allow_summary_lag=label == "oracle")
        verify_expected(state, expected_linux(case["xattr_before"]))
        if state["accounting"]["Free inodes"] != case["xattr_before"]["accounting"]["Free inodes"] - 3:
            raise RuntimeError("Linux attribute roundtrip inode accounting differs")
        state.pop("oracle_summary_lag")
        states.append(state)
    if states[0] != states[1]:
        raise RuntimeError("Core and independent attribute recovery disagree")
    clean = digest(image)
    run([recover, "--write", image])
    if digest(image) != clean:
        raise RuntimeError("Repeated attribute recovery changed the clean image")
    manifest = write_core_expectations(states[0], output / f"core-{image.stem}-values")
    run([reader, "--verify", image, manifest])
    returned = output / f"returned-{image.name}"
    run([reader, "--roundtrip", returned, image, manifest])
    after = snapshot(returned, output / f"returned-{image.stem}-checked", tools, run)
    wanted = copy.deepcopy(states[0]["objects"])
    changed = wanted[FILE]
    changed["attrs"]["user.binary"] = returned_payload(700).hex()
    changed["attrs"]["user.return"] = returned_payload(13).hex()
    for name in ("user.empty", "system.posix_acl_access", "security.capability"):
        del changed["attrs"][name]
    changed["inode"].update(mode=0o600, uid=54321, gid=65432, ctime=(RETURN_SECONDS, 0))
    verify_expected(after, wanted, newly_created=False)
    return dict(linux_authored_transactions=int(transactions[1]), reverse_state=states[0],
                returned_image=str(returned), returned_sha256=digest(returned), returned_state=after)
