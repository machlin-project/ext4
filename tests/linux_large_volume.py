"""Sparse-image Linux/core/Linux roundtrips above 32-bit physical addresses."""

import json
from pathlib import Path
import re
import stat
import subprocess

from check_large_volumes import compare_accounting, file_contents, snapshot as volume_snapshot
from generate_fixtures import resolve_tools
from linux_large_file import bounded_slice
from sparse_image import sparse_copy, sparse_digest


def snapshot(case, image, output, tools, run):
    tools = tools if isinstance(tools, dict) else resolve_tools(tools)
    return volume_snapshot(image, case["large_volume"], case.get("volume_phase", "mutated"),
                           tools, run, output)


def prepare(case, tree, tools, verify_only):
    image = Path(case["image"])
    if sparse_digest(image) != case["input_sha256"]:
        raise RuntimeError("Sparse large-volume source changed")
    commands = []

    def run(command):
        done = subprocess.run([str(x) for x in command], capture_output=True, text=True,
                              errors="backslashreplace", timeout=300)
        commands.append(dict(command=[str(x) for x in command], status=done.returncode,
                             stdout=done.stdout, stderr=done.stderr))
        (tree.parent / f"{tree.name}-expectations.json").write_text(json.dumps(commands, indent=2) + "\n")
        if done.returncode:
            raise RuntimeError(f"High-address native preparation failed: {command}: {done.stderr}")
        return done.stdout

    state = snapshot(case, image, tree.parent / f"{tree.name}-snapshot", tools, run)
    case["volume_before"] = state
    expected = tree / "expected"
    expected.mkdir()
    files, slices, attributes, inodes = [], [], [], []
    fixture = case["large_volume"]
    block = fixture["block_size"]
    for name, item in state["files"].items():
        path = f"/upper/{name}"
        inode = item["inode"]
        files.append(f"/mnt{path} {inode['inode']} {inode['size']} {inode['blocks']} "
                     f"{inode['mode']:o} {inode['uid']} {inode['gid']}")
        inodes.append(f"{path} {inode['inode']} {inode['mode'] | stat.S_IFREG:o} "
                      f"{inode['uid']} {inode['gid']} {inode['size']} 1")
        attribute = "user.native" if name == "native" else "user.large"
        filename = f"attribute-{len(attributes)}"
        (expected / filename).write_bytes(bytes.fromhex(item["value"]))
        attributes.append(f"{path} {attribute.encode().hex()} /expected/{filename} 0")
        spans, size, _, _ = file_contents(fixture, name, case.get("volume_phase", "mutated"))
        spans = [dict(offset=off, data=data.hex()) for off, data in spans]
        offsets = {0, size // 3, max(0, size - block), 16 * block + 17}
        for span in spans:
            offsets.add(max(0, span["offset"] - 11))
            offsets.add(span["offset"] + len(bytes.fromhex(span["data"])))
        for offset in sorted(offsets):
            if offset >= size:
                continue
            value = bounded_slice(spans, offset, min(block, size - offset))
            filename = f"slice-{len(slices)}"
            (expected / filename).write_bytes(value)
            slices.append(f"/mnt{path} {offset} /expected/{filename}")
    for filename, lines in (("large-file-inodes", files), ("large-file-slices", slices),
                            ("xattr-inodes", inodes), ("xattr-values", attributes), ("xattr-data", [])):
        (tree / filename).write_text("\n".join(lines) + "\n")
    (tree / "large-volume-geometry").write_text(f"{block} {block * fixture['cluster_blocks']} {int(verify_only)}\n")
    if sparse_digest(image) != case["input_sha256"]:
        raise RuntimeError("Large-volume expectation generation changed its source")


def verify(case, image, output, tools, recover, reader, run, native_replay):
    if native_replay is None:
        raise RuntimeError("High-address orphan cleanup requires native Linux recovery")
    pending = output / f"linux-pending-{image.name}"
    oracle = output / f"linux-reference-{image.name}"
    sparse_copy(image, pending)
    pending_hash = sparse_digest(pending)
    sparse_copy(image, oracle)
    replay = run([recover, "--write", image])
    transactions = re.search(r"transactions=(\d+)", replay)
    orphans = re.search(r"orphans=(\d+)", replay)
    if transactions is None or int(transactions[1]) == 0 or orphans is None or int(orphans[1]) != 1:
        raise RuntimeError("High-address roundtrip must recover a Linux transaction and one orphan")
    states = []
    for label, candidate in (("core", image), ("oracle", oracle)):
        if label == "oracle":
            native_replay(candidate, image, "replay")
        state = snapshot(dict(case, volume_phase="linux"), candidate,
                         output / f"{label}-{image.stem}-checked", tools, run)
        compare_accounting(case["volume_before"], state)
        states.append(state)
    if states[0] != states[1]:
        raise RuntimeError("Core and native Linux high-address recovery differ")
    clean_hash = sparse_digest(image)
    run([recover, "--write", image])
    if sparse_digest(image) != clean_hash:
        raise RuntimeError("Repeated high-address recovery changed a clean image")
    returned = output / f"returned-{image.name}"
    sparse_copy(image, returned)
    run([reader, "--linux-return", returned])
    after = snapshot(dict(case, volume_phase="returned"), returned,
                     output / f"returned-{image.stem}-checked", tools, run)
    compare_accounting(states[0], after)

    # Also mount the portable writer's committed high-address journal directly.
    source = Path(case["core_pending"])
    reference = Path(case["core_recovered"])
    if sparse_digest(source) != case["core_pending_sha256"] or sparse_digest(reference) != case["core_recovered_sha256"]:
        raise RuntimeError("Protected core high-address recovery inputs changed")
    candidate = output / f"core-pending-{image.name}"
    sparse_copy(source, candidate)
    native_replay(candidate, reference, "core-journal")
    committed_case = dict(case, volume_phase="committed")
    observed = snapshot(committed_case, candidate, output / f"core-journal-{image.stem}-checked", tools, run)
    expected = snapshot(committed_case, reference, output / f"core-journal-{image.stem}-expected", tools, run)
    if observed != expected:
        raise RuntimeError("Linux replay changed the core's committed high-address state")
    if sparse_digest(pending) != pending_hash or sparse_digest(source) != case["core_pending_sha256"]:
        raise RuntimeError("Protected high-address pending journal changed during checking")
    return dict(linux_authored_transactions=int(transactions[1]), linux_orphans_cleaned=int(orphans[1]),
                reverse_state=states[0], independent_replay="native Linux with strict nonrepairing e2fsck",
                image_digest_format="ext4-test-sparse-pages-v1", core_journal_linux_replay=True,
                linux_pending_image=str(pending), linux_pending_sha256=pending_hash,
                returned_image=str(returned), returned_sha256=sparse_digest(returned), returned_state=after)
