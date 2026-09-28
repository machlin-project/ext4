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
CREATED_FILES = 12
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


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def long_name(index):
    prefix = f"entry-{index:02d}-"
    return prefix + chr(ord("a") + index) * (LONG_NAME_BYTES - len(prefix))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tools-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--direct-replay", action="store_true",
                        help="also require e2fsck's direct fast replay; reproduces its directory-growth defect")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    build = args.tools_root.resolve()
    tools = resolve_tools(build)
    helper = output / "fast-commit-fixture"
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
    row.update(passed=True, helper_sha256=digest(helper))
    for name, block, added, removed in PROFILES:
        directory = output / name
        directory.mkdir()
        root = directory / "root"
        root.mkdir()
        original_data = b"A" * (SOURCE_BLOCKS * block)
        (root / "hello.txt").write_bytes(original_data)
        (root / "hello.txt").chmod(0o640)
        modern_orphans = "orphan_file" in added
        special_files = name.startswith("special-")
        if modern_orphans:
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
        run(row, [tools["mke2fs"], "-F", "-t", "ext4", "-b", block, "-N", 256,
                  "-I", 256, "-m", 0, "-O", "none," + ",".join(sorted(features)),
                  "-U", UUID, "-J", "size=8,fast_commit_size=256",
                  "-E", "lazy_itable_init=0,nodiscard", "-d", root, before, IMAGE_BYTES // block])
        run(row, [tools["e2fsck"], "-fn", before])
        shutil.copyfile(before, expected)
        shutil.copyfile(before, pending)
        commands = ["punch /hello.txt 1 1", "mkdir /new-dir"]
        # Reserve exactly the blocks needed by the long-name directory.
        entry_bytes = 8 + ((LONG_NAME_BYTES + 3) // 4) * 4
        tail_bytes = 12 if "metadata_csum" in features else 0
        remaining = block - tail_bytes - 24
        extra_blocks = 0
        for _ in range(CREATED_FILES):
            if entry_bytes > remaining:
                extra_blocks += 1
                remaining = block - tail_bytes
            remaining -= entry_bytes
        commands += ["expand_dir /new-dir"] * extra_blocks
        final_files = {"renamed": original_data[:block] + bytes(block) + original_data[2 * block:]}
        final_files["alias"] = final_files["renamed"]
        for index in range(CREATED_FILES):
            filename = long_name(index)
            payload = directory / f"data-{index:02d}"
            contents = bytes([ord("a") + index]) * (block + 17 + index)
            payload.write_bytes(contents)
            commands += [f'write "{payload}" /new-dir/{filename}',
                         f"sif /new-dir/{filename} mode 0100604"]
            final_files[f"new-dir/{filename}"] = contents
        commands += ["ln /hello.txt /alias", "ln /hello.txt /renamed",
                     "unlink /hello.txt", "sif /renamed links_count 2"]
        symlinks = ({"link-short": "renamed", "link-59": "a" * 59, "link-60": "b" * 60}
                    if special_files else {})
        commands += [f"symlink /{path} {target}" for path, target in symlinks.items()]
        if modern_orphans:
            # Allocate all other new names before releasing inode numbers: only
            # victim's old generation may be reused by this fast-commit stream.
            replacement = directory / "replacement-data"
            replacement_data = b"R" * (block + 37)
            replacement.write_bytes(replacement_data)
            commands += ["rm /victim", f'write "{replacement}" /reused',
                         "sif /reused mode 0100640", "sif /reused generation 123456789",
                         "rm /final-delete", "rm /orphan-held", "rm /legacy",
                         "punch /orphan-truncate 2 4",
                         f"sif /orphan-truncate size {block + 13}"]
            final_files.update(reused=replacement_data,
                               **{"orphan-truncate": b"O" * (block + 13)})
        script = directory / "expected.debugfs"
        script.write_text("\n".join(commands) + "\n")
        run(row, [tools["debugfs"], "-w", "-f", script, expected])
        if special_files:
            run(row, [helper, "--create-specials", expected])
        run(row, [tools["e2fsck"], "-fn", expected])
        row["serialization"] = run(row, [helper, pending, expected] +
                                   (["--specials"] if special_files else []))
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
            for damage in MALFORMED:
                candidate = directory / (damage + ".img")
                shutil.copyfile(before, candidate)
                run(row, [helper, candidate, expected, "--" + damage])
                row["malformed"][damage] = digest(candidate)
        row.update(passed=True, expected_sha256=digest(expected), journal_sha256=digest(journal),
                   **wanted, direct_replay=args.direct_replay)
        if args.direct_replay:
            row["oracle_sha256"] = digest(oracle)
        save()
        print(f"PASS fast commit {name}: independent pending and expected filesystems", flush=True)


if __name__ == "__main__":
    main()
