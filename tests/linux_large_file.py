"""Bounded Linux/core/Linux checks for sparse files at their format size ceiling."""

import json
from pathlib import Path
import re
import shutil
import subprocess

from check_large_files import SECTOR_BYTES, expected_created, span_blocks, verify_file
from check_namespace import inode_fields
from check_orphans import accounting, digest
from check_rename import entries
from generate_fixtures import resolve_tools

BYTE_BOUNDARY = 1 << 32


def expected_files(fixture, phase):
    seed = [(row["offset"], bytes.fromhex(row["data"])) for row in fixture["segments"]]
    created, limit, allocated = expected_created(fixture, "written")
    result = {"/seed": (seed, fixture["limit"], span_blocks(seed, fixture["block_size"]))}
    if phase != "written":
        created[2] = (created[2][0], b"L" * 7)
        created[-1] = (created[-1][0], b"N")
        native = [(0, b"\x64" * 61), (BYTE_BOUNDARY - 3, b"V" * 7), (limit - 1, b"X")]
        if phase == "returned":
            created, limit, allocated = [], 0, set()
            native[1] = (BYTE_BOUNDARY - 3, b"V" * 4)
            native[2] = (fixture["limit"] - 1, b"Y")
            native.append(((1 << 31) - 3, b"W" * 7))
        result["/linux-large"] = (native, fixture["limit"], span_blocks(native, fixture["block_size"]))
    result["/created"] = (created, limit, allocated)
    return result


def snapshot(case, image, output, tools, run):
    fixture = case["large_files"]
    phase = case.get("large_phase", "written")
    if phase not in ("written", "linux", "returned"):
        raise RuntimeError("Unknown large-file native phase")
    tools = tools if isinstance(tools, dict) else resolve_tools(tools)
    output.mkdir(parents=True, exist_ok=False)
    run([tools["e2fsck"], "-fn", image])
    counts = accounting(run([tools["dumpe2fs"], "-h", image]))
    files = {}
    for path, (spans, size, allocated) in expected_files(fixture, phase).items():
        inode = verify_file(image, path, fixture, spans, size, allocated, tools, run)
        if path == "/linux-large" and (inode["uid"], inode["gid"], inode["mode"]) != (0, 0, 0o640):
            raise RuntimeError("Linux-created large file has unexpected ownership/mode")
        if path == "/created" and (inode["uid"], inode["gid"], inode["mode"]) != (70000, 80000, 0o640):
            raise RuntimeError("Large-file mutation changed admitted ownership/mode")
        files[path] = dict(inode=inode, spans=[dict(offset=off, data=value.hex()) for off, value in spans])
    root = inode_fields(run([tools["debugfs"], "-R", "stat /", image]))
    names = entries(run([tools["debugfs"], "-R", "ls -p /", image]))
    if set(names) != {".", "..", "lost+found"} | {path[1:] for path in files}:
        raise RuntimeError("Large-file namespace differs, including orphan cleanup")
    for path, record in files.items():
        if names[path[1:]] != record["inode"]["inode"]:
            raise RuntimeError("Large-file directory identity differs")
    state = dict(files=files, root=root, names=names, accounting=counts)
    (output / "state.json").write_text(json.dumps(state, indent=2) + "\n")
    return state


def bounded_slice(spans, offset, size):
    result = bytearray(size)
    for record in spans:
        start = record["offset"]
        value = bytes.fromhex(record["data"])
        first, last = max(offset, start), min(offset + size, start + len(value))
        if last > first:
            result[first - offset:last - offset] = value[first - start:last - start]
    return bytes(result)


def prepare(case, tree, tools, verify_only):
    image = Path(case["image"])
    if digest(image) != case["input_sha256"]:
        raise RuntimeError("Large-file Linux source changed")
    commands = []

    def run(command):
        done = subprocess.run([str(x) for x in command], capture_output=True, text=True,
                              errors="backslashreplace", timeout=120)
        commands.append(dict(command=[str(x) for x in command], status=done.returncode,
                             stdout=done.stdout, stderr=done.stderr))
        (tree.parent / f"{tree.name}-expectations.json").write_text(json.dumps(commands, indent=2) + "\n")
        if done.returncode:
            raise RuntimeError(f"Large-file native preparation failed: {command}: {done.stderr}")
        return done.stdout

    state = snapshot(case, image, tree.parent / f"{tree.name}-snapshot", tools, run)
    case["large_before"] = state
    expected = tree / "expected"
    expected.mkdir()
    inode_lines, slices = [], []
    block = case["large_files"]["block_size"]
    for path, item in state["files"].items():
        inode = item["inode"]
        inode_lines.append(f"/mnt{path} {inode['inode']} {inode['size']} {inode['blocks']} "
                           f"{inode['mode']:o} {inode['uid']} {inode['gid']}")
        size = inode["size"]
        offsets = {0, size // 3, max(0, size - block), 16 * block + 17}
        for record in item["spans"]:
            offsets.add(max(record["offset"] - 11, 0))
            offsets.add(record["offset"] + len(bytes.fromhex(record["data"])))
        for offset in sorted(offsets):
            if offset >= size:
                continue
            value = bounded_slice(item["spans"], offset, min(block, size - offset))
            filename = f"slice-{len(slices)}"
            (expected / filename).write_bytes(value)
            slices.append(f"/mnt{path} {offset} /expected/{filename}")
    (tree / "large-file-inodes").write_text("\n".join(inode_lines) + "\n")
    (tree / "large-file-slices").write_text("\n".join(slices) + "\n")
    inline = case["large_files"]["profile"] in ("inline", "cluster-inline")
    (tree / "large-file-geometry").write_text(f"{block} {case['large_files']['limit']} {int(verify_only)} {int(inline)}\n")
    if digest(image) != case["input_sha256"]:
        raise RuntimeError("Large-file native preparation changed its source")


def compare_accounting(before, after, inode_delta):
    sectors = sum(row["inode"]["blocks"] for row in after["files"].values()) + after["root"]["blocks"]
    sectors -= sum(row["inode"]["blocks"] for row in before["files"].values()) + before["root"]["blocks"]
    expected = dict(before["accounting"])
    block = expected["Block size"]
    if sectors * SECTOR_BYTES % block:
        raise RuntimeError("Large-file Linux sector charge is not block-aligned")
    expected["Free blocks"] -= sectors * SECTOR_BYTES // block
    expected["Free inodes"] += inode_delta
    if after["accounting"] != expected:
        raise RuntimeError(f"Large-file allocation differs: actual={after['accounting']}, expected={expected}")
    if before["files"]["/seed"] != after["files"]["/seed"]:
        raise RuntimeError("Large-file roundtrip changed its independently authored seed")


def verify(case, image, output, tools, recover, reader, run, native_replay):
    if native_replay is None:
        raise RuntimeError("Large-file orphan cleanup requires native Linux recovery")
    pending = output / f"linux-pending-{image.name}"
    oracle = output / f"linux-reference-{image.name}"
    shutil.copyfile(image, pending)
    pending_hash = digest(pending)
    shutil.copyfile(image, oracle)
    replay = run([recover, "--write", image])
    transactions = re.search(r"transactions=(\d+)", replay)
    orphans = re.search(r"orphans=(\d+)", replay)
    if transactions is None or int(transactions[1]) == 0 or orphans is None or int(orphans[1]) != 1:
        raise RuntimeError("Large-file reverse roundtrip must recover a Linux transaction and one orphan")
    states = []
    for label, candidate in (("core", image), ("oracle", oracle)):
        if label == "oracle":
            native_replay(candidate, image, "replay")
        state = snapshot(dict(case, large_phase="linux"), candidate,
                         output / f"{label}-{image.stem}-checked", tools, run)
        compare_accounting(case["large_before"], state, -1)
        states.append(state)
    if states[0] != states[1]:
        raise RuntimeError("Core and native Linux high-offset recovery differ")
    clean_hash = digest(image)
    run([recover, "--write", image])
    if digest(image) != clean_hash:
        raise RuntimeError("Repeated large-file recovery changed a clean image")
    returned = output / f"returned-{image.name}"
    run([reader, "--linux-return", returned, image])
    after = snapshot(dict(case, large_phase="returned"), returned,
                     output / f"returned-{image.stem}-checked", tools, run)
    compare_accounting(states[0], after, 0)
    if digest(pending) != pending_hash:
        raise RuntimeError("Linux pending large-file journal changed during checking")
    return dict(linux_authored_transactions=int(transactions[1]), linux_orphans_cleaned=int(orphans[1]),
                reverse_state=states[0], independent_replay="native Linux with strict nonrepairing e2fsck",
                linux_pending_image=str(pending), linux_pending_sha256=pending_hash,
                returned_image=str(returned), returned_sha256=digest(returned), returned_state=after)
