#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Author fast-commit inputs and expected filesystems independently of the core."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess

from generate_fixtures import EXPECTED_FEATURES, UUID, resolve_tools
from fast_commit_reference import read_namespace

IMAGE_BYTES = 32 * 1024 * 1024
SOURCE_BLOCKS = 5
DIRECT_BLOCKS = 12
POINTER_BYTES = 4
CREATED_FILES = 12
LARGE_PREFIX_FILES = {1024: 256, 4096: 1024}
# Enough long-name entries and 1 KiB inode-table blocks that one conversion
# transaction exceeds the ordinary 256-snapshot bound.
HUGE_PREFIX_FILES = 1024
FAST_COMMIT_KIB = {"large-prefix": 1024, "huge-prefix": 2048}
LONG_NAME_BYTES = 230
SEQUENCE = 7
COMMITS = 3
PROFILES = (
    ("1k", 1024, set(), set()),
    ("4k", 4096, set(), set()),
    ("checksum-seed", 4096, {"metadata_csum_seed"}, set()),
    ("no-checksum", 1024, set(), {"metadata_csum"}),
    ("orphan-1k", 1024, {"orphan_file"}, set()),
    ("orphan-4k", 4096, {"orphan_file"}, set()),
    ("special-1k", 1024, set(), set()),
    ("special-4k", 4096, set(), set()),
    ("xattr-reuse-1k", 1024, {"orphan_file", "ea_inode"}, set()),
    ("xattr-reuse-4k", 4096, {"orphan_file", "ea_inode"}, set()),
    ("xattr-reuse-legacy-1k", 1024, {"ea_inode"}, set()),
    ("indirect-1k", 1024, set(), {"extent", "64bit"}),
    ("indirect-4k", 4096, set(), {"extent", "64bit"}),
    ("large-prefix-1k", 1024, set(), set()),
    ("large-prefix-4k", 4096, set(), set()),
    ("huge-prefix-1k", 1024, set(), set()),
)

SPECIAL_NODES = {
    "character-legacy": ("character", 255, 255),
    "character-wide": ("character", 256, 256),
    "character-zero": ("character", 0, 0),
    "block-wide": ("block", 4095, 1048575),
    "fifo": ("FIFO", 0, 0),
    "socket": ("socket", 0, 0),
}
MALFORMED = ("bad-link-size", "bad-link-nul", "bad-link-terminator", "bad-special-size",
             "missing-link-range")
INDIRECT_MALFORMED = ("indirect-unwritten", "indirect-logical-limit")


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def long_name(index):
    prefix = f"entry-{index:02d}-"
    return prefix + chr(ord("a") + index % 26) * (LONG_NAME_BYTES - len(prefix))


def attribute_value(index, block_size):
    size = 65536 if index == 0 else 3 * block_size + 7
    return bytes((position * 17 + index * 31) & 255 for position in range(size))


def indirect_data(block_size):
    per_block = block_size // POINTER_BYTES
    blocks = DIRECT_BLOCKS + per_block + 2
    contents = bytearray(blocks * block_size)
    for logical in (0, 1, DIRECT_BLOCKS - 1, DIRECT_BLOCKS,
                    DIRECT_BLOCKS + per_block - 1, blocks - 2, blocks - 1):
        contents[logical * block_size:(logical + 1) * block_size] = b"A" * block_size
    return bytes(contents)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tools-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--direct-replay", action="store_true",
                        help="also require e2fsck's direct fast replay; reproduces its directory-growth defect")
    parser.add_argument("--profile", action="append", choices=[name for name, *_ in PROFILES],
                        help="generate only the selected profiles; default: all")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    build = args.tools_root.resolve()
    tools = resolve_tools(build)
    helper = output / "fast-commit-fixture"
    attribute_helper = output / "ea-inode-fixture"
    rows = []

    def save():
        (output / "report.json").write_text(json.dumps(rows, indent=2) + "\n")

    def run(row, command, allowed=(0,)):
        command = [str(part) for part in command]
        done = subprocess.run(command, capture_output=True, text=True, timeout=300)
        row["commands"].append(dict(command=command, status=done.returncode,
                                    stdout=done.stdout, stderr=done.stderr))
        save()
        if done.returncode not in allowed:
            raise RuntimeError(f"Fast-commit fixture command failed: {command}: {done.stderr}")
        return done.stdout

    row = dict(kind="helper-build", commands=[], passed=False)
    rows.append(row)
    run(row, shlex.split(os.environ.get("CC", "cc")) + [
        "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-Wdeclaration-after-statement",
        f"-I{build}", f"-I{build / 'lib'}", f"-I{build.parent / 'lib'}",
        Path(__file__).with_name("fast_commit_fixture.c"),
        build / "lib/libext2fs.a", build / "lib/libcom_err.a", "-lpthread", "-o", helper,
    ])
    run(row, shlex.split(os.environ.get("CC", "cc")) + [
        "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-Wdeclaration-after-statement",
        f"-I{build / 'lib'}", f"-I{build.parent / 'lib'}",
        Path(__file__).with_name("ea_inode_fixture.c"),
        build / "lib/libext2fs.a", build / "lib/libcom_err.a", "-lpthread", "-o", attribute_helper,
    ])
    row.update(passed=True, helper_sha256=digest(helper))
    for name, block, added, removed in PROFILES:
        if args.profile and name not in args.profile:
            continue
        directory = output / name
        directory.mkdir()
        root = directory / "root"
        root.mkdir()
        indirect = "extent" in removed
        huge_prefix = name.startswith("huge-prefix-")
        large_prefix = name.startswith("large-prefix-") or huge_prefix
        created_files = (HUGE_PREFIX_FILES if huge_prefix else
                         LARGE_PREFIX_FILES[block] if large_prefix else CREATED_FILES)
        fast_commit_kib = FAST_COMMIT_KIB[name.rsplit("-", 1)[0]] if large_prefix else 256
        original_data = indirect_data(block) if indirect else b"A" * (SOURCE_BLOCKS * block)
        (root / "hello.txt").write_bytes(original_data)
        (root / "hello.txt").chmod(0o640)
        modern_orphans = "orphan_file" in added
        special_files = name.startswith("special-") or indirect
        xattr_reuse = "ea_inode" in added
        orphans = modern_orphans or xattr_reuse or indirect
        if xattr_reuse:
            (root / "keep-xattrs").write_bytes(b"surviving attribute owner")
            (root / "keep-xattrs").chmod(0o640)
            (root / "victim-shared").write_bytes(b"second attribute owner")
            (root / "victim-shared").chmod(0o640)
        if orphans:
            for filename in ("victim", "final-delete", "orphan-held", "legacy", "orphan-truncate"):
                (root / filename).write_bytes(b"O" * (SOURCE_BLOCKS * block))
                (root / filename).chmod(0o640)
        before = directory / "before.img"
        expected = directory / "expected.img"
        pending = directory / "pending.img"
        oracle = directory / "e2fsck-recovered.img"
        journal = directory / "pending.journal"
        row = dict(profile=name, block_size=block, sequence=SEQUENCE, commits=COMMITS,
                   commands=[], passed=False)
        rows.append(row)
        features = (EXPECTED_FEATURES | {"fast_commit"} | added) - removed
        run(row, [tools["mke2fs"], "-F", "-t", "ext4", "-b", block,
                  "-N", max(256, created_files + 64),
                  "-I", 256, "-m", 0, "-O", "none," + ",".join(sorted(features)),
                  "-U", UUID, "-J", f"size=8,fast_commit_size={fast_commit_kib}",
                  "-E", "lazy_itable_init=0,nodiscard", "-d", root, before, IMAGE_BYTES // block])
        if indirect:
            row["old_mapping"] = run(row, [helper, "--prepare-indirect", before])
        if xattr_reuse:
            for index in range(8):
                payload = directory / f"attribute-{index}.value"
                payload.write_bytes(attribute_value(index, block))
                run(row, [attribute_helper, before, "/victim", f"user.value{index}", payload])
            for target in ("/keep-xattrs", "/victim-shared"):
                shared = run(row, [helper, "--share-xattrs", before, target])
                if shared.strip() != "shared-body=1 private-body=2 shared-external=5":
                    raise RuntimeError("Unexpected attribute-sharing fixture layout")
        run(row, [tools["e2fsck"], "-fn", before])
        shutil.copyfile(before, expected)
        shutil.copyfile(before, pending)
        if special_files and orphans:
            # Keep special inodes out of the numbers released by orphan cleanup;
            # only the explicitly logged victim replacement changes generation.
            run(row, [helper, "--create-specials", expected])
        if xattr_reuse:
            for owner in ("/victim", "/victim-shared"):
                run(row, [helper, "--detach-shared-xattrs", expected, owner])
        commands = ["punch /hello.txt 1 1", "mkdir /new-dir"]
        # Reserve exactly the blocks needed by the long-name directory.
        entry_bytes = 8 + ((LONG_NAME_BYTES + 3) // 4) * 4
        tail_bytes = 12 if "metadata_csum" in features else 0
        remaining = block - tail_bytes - 24
        extra_blocks = 0
        for _ in range(created_files):
            if entry_bytes > remaining:
                extra_blocks += 1
                remaining = block - tail_bytes
            remaining -= entry_bytes
        commands += ["expand_dir /new-dir"] * extra_blocks
        final_files = {"renamed": original_data[:block] + bytes(block) + original_data[2 * block:]}
        final_files["alias"] = final_files["renamed"]
        for index in range(created_files):
            filename = long_name(index)
            payload = directory / f"data-{index:02d}"
            contents = bytes([ord("a") + index % 26]) * (block + 17 + index)
            if indirect and index == 0:
                contents = b"a" * ((DIRECT_BLOCKS + 1) * block + 17)
            payload.write_bytes(contents)
            commands += [f'write "{payload}" /new-dir/{filename}',
                         f"sif /new-dir/{filename} mode 0100604"]
            final_files[f"new-dir/{filename}"] = contents
        commands += ["ln /hello.txt /alias", "ln /hello.txt /renamed",
                     "unlink /hello.txt", "sif /renamed links_count 2"]
        symlinks = ({"link-short": "renamed", "link-59": "a" * 59, "link-60": "b" * 60}
                    if special_files else {})
        commands += [f"symlink /{path} {target}" for path, target in symlinks.items()]
        if orphans:
            # Allocate unrelated new names before releasing inode numbers. Each
            # replacement must reuse its intended old attribute owner.
            replacement = directory / "replacement-data"
            replacement_data = b"R" * (block + 37)
            replacement.write_bytes(replacement_data)
            if xattr_reuse:
                commands += [f"ea_rm /victim user.value{index}" for index in range(3)]
                final_files["keep-xattrs"] = (root / "keep-xattrs").read_bytes()
            commands += ["rm /victim", f'write "{replacement}" /reused',
                         "sif /reused mode 0100640", "sif /reused generation 123456789"]
            if xattr_reuse:
                commands += ["ea_rm /victim-shared user.value0", "rm /victim-shared",
                             f'write "{replacement}" /reused-shared',
                             "sif /reused-shared mode 0100640",
                             "sif /reused-shared generation 987654321"]
                final_files["reused-shared"] = replacement_data
            commands += ["rm /final-delete", "rm /orphan-held", "rm /legacy",
                         "punch /orphan-truncate 2 4",
                         f"sif /orphan-truncate size {block + 13}"]
            final_files.update(reused=replacement_data,
                               **{"orphan-truncate": b"O" * (block + 13)})
        script = directory / "expected.debugfs"
        script.write_text("\n".join(commands) + "\n")
        run(row, [tools["debugfs"], "-w", "-f", script, expected])
        if special_files and not orphans:
            run(row, [helper, "--create-specials", expected])
        run(row, [tools["e2fsck"], "-fn", expected])
        row["serialization"] = run(row, [helper, pending, expected] +
                                   (["--specials"] if special_files else
                                    ["--huge-prefix"] if huge_prefix else
                                    ["--large-prefix"] if large_prefix else []))
        if f"sequence={SEQUENCE} commits={COMMITS} " not in row["serialization"]:
            raise RuntimeError("Unexpected serialized fast-commit inventory")
        run(row, [tools["debugfs"], "-R", f'dump <8> "{journal}"', pending])
        row["pending_sha256"] = digest(pending)
        references = [("expected", expected)]
        if args.direct_replay:
            shutil.copyfile(pending, oracle)
            run(row, [tools["e2fsck"], "-fy", "-E", "journal_only", oracle])
            run(row, [tools["e2fsck"], "-fn", oracle])
            references.append(("e2fsck", oracle))
        wanted = dict(files={path: hashlib.sha256(contents).hexdigest()
                             for path, contents in final_files.items()},
                      directories=["lost+found", "new-dir"],
                      symlinks={path: value.encode().hex() for path, value in symlinks.items()},
                      xattrs={"keep-xattrs": {f"user.value{index}":
                              hashlib.sha256(attribute_value(index, block)).hexdigest()
                              for index in (0, 3, 4, 5, 6, 7)}} if xattr_reuse else {},
                      special={path: dict(type=kind, mode=0o640, uid=0, gid=0, links=1,
                                          device_major=major, device_minor=minor)
                               for path, (kind, major, minor) in SPECIAL_NODES.items()}
                      if special_files else {})
        for label, image in references:
            exported = directory / (label + "-files")
            exported.mkdir()
            actual = read_namespace(image, exported, block, tools["debugfs"],
                                    lambda command: run(row, command))
            if actual != wanted:
                raise RuntimeError(f"{name}: {label} namespace or data differs from expected")
        if digest(pending) != row["pending_sha256"]:
            raise RuntimeError("Protected pending input changed")
        if special_files:
            row["malformed"] = {}
            for damage in MALFORMED + (INDIRECT_MALFORMED if indirect else ()):
                candidate = directory / (damage + ".img")
                shutil.copyfile(before, candidate)
                run(row, [helper, candidate, expected, "--" + damage])
                row["malformed"][damage] = digest(candidate)
        if large_prefix and not huge_prefix:
            candidate = directory / "conflicting-name-owner.img"
            shutil.copyfile(before, candidate)
            run(row, [helper, candidate, expected, "--large-prefix-conflict"])
            row["malformed"] = {"conflicting-name-owner": digest(candidate)}
        row.update(passed=True, expected_sha256=digest(expected), journal_sha256=digest(journal),
                   created_files=created_files,
                   **wanted, direct_replay=args.direct_replay)
        if args.direct_replay:
            row["oracle_sha256"] = digest(oracle)
        save()
        print(f"PASS fast commit {name}: independent pending and expected filesystems", flush=True)


if __name__ == "__main__":
    main()
