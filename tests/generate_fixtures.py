#!/usr/bin/env python3
"""Generate deterministic ext4 fixture data and images with e2fsprogs.

The input tree and file bytes are deterministic. Whole image bytes are not:
mke2fs writes timestamps and other time-dependent metadata.
"""

from __future__ import annotations

import argparse
import os
import shlex
import shutil
import subprocess
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT = REPOSITORY / "artifacts" / "fixtures"
TOOL_PATHS = {
    "mke2fs": Path("misc/mke2fs"),
    "e2fsck": Path("e2fsck/e2fsck"),
    "debugfs": Path("debugfs/debugfs"),
    "dumpe2fs": Path("misc/dumpe2fs"),
}
UUID = "2e3fadb8-46b1-4c67-9545-f0d317e8cd57"
FEATURE_LIST = (
    "has_journal,ext_attr,resize_inode,dir_index,filetype,extent,64bit,"
    "flex_bg,sparse_super,large_file,huge_file,dir_nlink,extra_isize,metadata_csum"
)
EXPECTED_FEATURES = set(FEATURE_LIST.split(","))
NANOSECONDS_PER_SECOND = 1_000_000_000
LOW_SECONDS_BITS = 32
LOW_SECONDS_MODULUS = 1 << LOW_SECONDS_BITS
LOW_SECONDS_SIGN_BIT = 1 << (LOW_SECONDS_BITS - 1)
EPOCH_BITS = 2
EPOCH_MASK = (1 << EPOCH_BITS) - 1
METADATA_TIMES = {
    "atime": (-1, 123456789),
    "mtime": (2147483648, 987654321),
    "ctime": (4294967296, 42),
    "crtime": (1700000000, 999999999),
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--tools-root",
        type=Path,
        help=(
            "e2fsprogs out-of-tree build directory containing misc/mke2fs, "
            "e2fsck/e2fsck, debugfs/debugfs, and misc/dumpe2fs; when omitted, "
            "all four tools must be available on PATH"
        ),
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=DEFAULT_OUTPUT,
        help=f"output directory (default: {DEFAULT_OUTPUT})",
    )
    return parser.parse_args()


def resolve_tools(tools_root: Path | None) -> dict[str, Path]:
    resolved: dict[str, Path] = {}
    if tools_root is not None:
        build_root = tools_root.expanduser().resolve()
        for name, relative_path in TOOL_PATHS.items():
            tool = build_root / relative_path
            if not tool.is_file() or not os.access(tool, os.X_OK):
                raise FileNotFoundError(f"Missing executable {name}: {tool}")
            resolved[name] = tool
        return resolved

    for name in TOOL_PATHS:
        found = shutil.which(name)
        if found is None:
            raise FileNotFoundError(
                f"{name} was not found on PATH; pass --tools-root with an e2fsprogs build"
            )
        resolved[name] = Path(found).resolve()
    return resolved


def tool_version(tool: Path) -> str:
    completed = subprocess.run(
        [str(tool), "-V"],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        check=False,
    )
    if completed.returncode != 0:
        raise RuntimeError(
            f"Version query failed for {tool} with status {completed.returncode}: "
            f"{completed.stdout.strip()}"
        )
    return completed.stdout.rstrip()


def write_new(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("xb") as output:
        output.write(data)


def make_tree(root: Path) -> None:
    root.mkdir()
    write_new(root / "hello.txt", b"Machlin ext4\n")
    write_new(root / "nested" / "child.txt", b"nested data\n")
    write_new(root / "empty", b"")
    write_new(root / "metadata.txt", b"metadata\n")
    os.link(root / "hello.txt", root / "hello-hardlink")
    os.symlink("hello.txt", root / "hello-link")
    os.symlink("L" * 100, root / "long-link")

    payload = bytes((index * 17 + 23) & 0xFF for index in range(200000))
    write_new(root / "payload.bin", payload)

    sparse_path = root / "sparse.bin"
    with sparse_path.open("xb") as sparse_file:
        sparse_file.truncate(2 * 1024 * 1024)
        for extent_index in range(12):
            sparse_file.seek(extent_index * 65536)
            sparse_file.write(bytes([extent_index + 1]) * 4096)

    many = root / "many"
    many.mkdir()
    for entry_index in range(400):
        write_new(
            many / f"entry-{entry_index:04d}",
            f"{entry_index}\n".encode("ascii"),
        )


def run_logged(
    output: Path,
    name: str,
    args: list[str],
    versions: dict[str, str],
    accepted_statuses: set[int] | None = None,
) -> str:
    tool = Path(args[0])
    tool_name = tool.name
    if tool_name not in versions:
        raise RuntimeError(f"No recorded version for executable {tool}")
    completed = subprocess.run(
        args,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        check=False,
    )
    log_path = output / f"{name}.log"
    with log_path.open("x", encoding="utf-8") as log:
        log.write(f"argv: {shlex.join(args)}\n")
        log.write(f"version argv: {shlex.join([str(tool), '-V'])}\n")
        log.write(f"version:\n{versions[tool_name]}\n")
        log.write(f"exit status: {completed.returncode}\n")
        log.write("--- output ---\n")
        log.write(completed.stdout)
    permitted = accepted_statuses if accepted_statuses is not None else {0}
    if completed.returncode not in permitted:
        raise RuntimeError(
            f"{name} failed with status {completed.returncode}; see {log_path}"
        )
    return completed.stdout


def encoded_inode_time(seconds: int, nanoseconds: int) -> tuple[int, int]:
    if not 0 <= nanoseconds < NANOSECONDS_PER_SECOND:
        raise ValueError(f"Invalid nanosecond value: {nanoseconds}")
    low_seconds = seconds & (LOW_SECONDS_MODULUS - 1)
    signed_low_seconds = (
        low_seconds
        if low_seconds < LOW_SECONDS_SIGN_BIT
        else low_seconds - LOW_SECONDS_MODULUS
    )
    epoch = ((seconds - signed_low_seconds) // LOW_SECONDS_MODULUS) & EPOCH_MASK
    extra = (nanoseconds << EPOCH_BITS) | epoch
    return low_seconds, extra


def set_metadata_inode(
    output: Path,
    image: Path,
    tools: dict[str, Path],
    versions: dict[str, str],
) -> None:
    fields = [
        ("uid", "70001"),
        ("gid", "80002"),
        ("mode", "0100640"),
    ]
    for name, (seconds, nanoseconds) in METADATA_TIMES.items():
        low_seconds, extra = encoded_inode_time(seconds, nanoseconds)
        fields.extend(
            [
                (f"{name}_lo", str(low_seconds)),
                (f"{name}_hi", str(extra)),
            ]
        )

    for field, value in fields:
        command = f"set_inode_field /metadata.txt {field} {value}"
        run_logged(
            output,
            f"debugfs-metadata-{image.stem}-{field}",
            [str(tools["debugfs"]), "-w", "-R", command, str(image)],
            versions,
        )

    run_logged(
        output,
        f"debugfs-metadata-stat-{image.stem}",
        [str(tools["debugfs"]), "-R", "stat /metadata.txt", str(image)],
        versions,
    )


def create_image(
    output: Path,
    root: Path,
    image: Path,
    block_size: int,
    tools: dict[str, Path],
    versions: dict[str, str],
) -> None:
    blocks = (64 * 1024 * 1024) // block_size
    run_logged(
        output,
        f"mke2fs-{image.stem}",
        [
            str(tools["mke2fs"]),
            "-F",
            "-t",
            "ext4",
            "-b",
            str(block_size),
            "-O",
            f"none,{FEATURE_LIST}",
            "-U",
            UUID,
            "-E",
            "lazy_itable_init=0,lazy_journal_init=0",
            "-d",
            str(root),
            str(image),
            str(blocks),
        ],
        versions,
    )
    set_metadata_inode(output, image, tools, versions)
    run_logged(
        output,
        f"e2fsck-{image.stem}",
        [str(tools["e2fsck"]), "-fn", str(image)],
        versions,
    )
    run_logged(
        output,
        f"dumpe2fs-{image.stem}",
        [str(tools["dumpe2fs"]), "-h", str(image)],
        versions,
    )
    run_logged(
        output,
        f"debugfs-sparse-{image.stem}",
        [str(tools["debugfs"]), "-R", "stat /sparse.bin", str(image)],
        versions,
    )


def record_features(
    output: Path,
    image: Path,
    tools: dict[str, Path],
    versions: dict[str, str],
) -> tuple[set[str], str]:
    text = run_logged(
        output,
        f"dumpe2fs-features-{image.stem}",
        [str(tools["dumpe2fs"]), "-h", str(image)],
        versions,
    )
    line = next(
        (item for item in text.splitlines() if item.startswith("Filesystem features:")),
        None,
    )
    if line is None:
        raise RuntimeError(f"Filesystem feature line missing for {image}")
    return set(line.split(":", 1)[1].split()), line


def main() -> None:
    args = parse_args()
    output = args.output.expanduser().resolve()
    output.mkdir(parents=True, exist_ok=True)
    root = output / "root"
    image_4k = output / "ext4-4k.img"
    image_1k = output / "ext4-1k.img"
    indexed_image = output / "ext4-indexed.img"
    existing = [path for path in (root, image_4k, image_1k, indexed_image) if path.exists()]
    if existing:
        paths = ", ".join(str(path) for path in existing)
        raise FileExistsError(f"Refusing to overwrite existing fixture data: {paths}")

    tools = resolve_tools(args.tools_root)
    versions = {name: tool_version(path) for name, path in tools.items()}
    (output / "tool-versions.txt").write_text(
        "\n".join(
            f"{name}: {shlex.join([str(path), '-V'])}\n{versions[name]}"
            for name, path in tools.items()
        )
        + "\n",
        encoding="utf-8",
    )

    make_tree(root)
    create_image(output, root, image_4k, 4096, tools, versions)
    create_image(output, root, image_1k, 1024, tools, versions)

    shutil.copyfile(image_4k, indexed_image)
    run_logged(
        output,
        "e2fsck-indexed-build",
        [str(tools["e2fsck"]), "-fyD", str(indexed_image)],
        versions,
        {0, 1},
    )
    run_logged(
        output,
        "e2fsck-indexed-verify",
        [str(tools["e2fsck"]), "-fn", str(indexed_image)],
        versions,
    )
    run_logged(
        output,
        "debugfs-many-indexed",
        [str(tools["debugfs"]), "-R", "stat /many", str(indexed_image)],
        versions,
    )

    report_lines = []
    feature_mismatch = False
    for image in (image_4k, image_1k, indexed_image):
        actual, line = record_features(output, image, tools, versions)
        extra = sorted(actual - EXPECTED_FEATURES)
        missing = sorted(EXPECTED_FEATURES - actual)
        report_lines.append(f"{image.name}: {line}")
        report_lines.append(f"  extra: {', '.join(extra) if extra else '(none)'}")
        report_lines.append(f"  missing: {', '.join(missing) if missing else '(none)'}")
        feature_mismatch |= actual != EXPECTED_FEATURES
    with (output / "features.txt").open("x", encoding="utf-8") as report:
        report.write("\n".join(report_lines) + "\n")
    if feature_mismatch:
        raise RuntimeError(f"Feature set differs from requested list; inspect {output / 'features.txt'}")

    print(f"Fixtures and images created under {output}")
    print(f"Feature report: {output / 'features.txt'}")


if __name__ == "__main__":
    main()
