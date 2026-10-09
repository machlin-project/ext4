#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Author fs-verity files independently of the core and store them with debugfs."""
import argparse
import hashlib
import json
from pathlib import Path
import random
import struct
import subprocess

from generate_fixtures import EXPECTED_FEATURES, UUID, resolve_tools

IMAGE_BYTES = 32 * 1024 * 1024
METADATA_ALIGNMENT = 65536
VERITY_VERSION = 1
ALGORITHMS = {1: ("sha256", 32, 64), 2: ("sha512", 64, 128)}
INODE_FLAGS_EXTENTS_VERITY = 0x00080000 | 0x00100000
DESCRIPTOR = struct.Struct("<BBBBIQ64s32s144s")
SIZE_FIELD = struct.Struct("<I")
PROFILES = (
    dict(name="1k-encrypted", block_size=1024, algorithm=1, salt=b"", sizes=(1, 4097),
         features={"encrypt"}),
    dict(name="4k-encrypted", block_size=4096, algorithm=1, salt=b"", sizes=(1, 4097),
         features={"encrypt"}),
    dict(name="4k-encrypted-orphan", block_size=4096, algorithm=1, salt=b"",
         sizes=(1, 4097), features={"encrypt", "orphan_file"}),
    dict(name="4k", block_size=4096, algorithm=1, salt=b"", sizes=(0, 100, 4096, 1536 * 1024 + 77)),
    dict(name="1k-salted", block_size=1024, algorithm=1, salt=bytes(range(16)),
         sizes=(1, 1024, 2560 * 1024 + 5)),
    dict(name="sha512", block_size=4096, algorithm=2, salt=bytes(range(32, 64)),
         sizes=(4095, 700 * 1024)),
    # Enabling verity converts small inline files to extents first.
    dict(name="4k-inline", block_size=4096, algorithm=1, salt=b"", sizes=(100, 600 * 1024),
         features={"inline_data"}),
    # More extents than one external leaf holds, with Merkle metadata beyond EOF.
    dict(name="4k-fragmented", block_size=4096, algorithm=1, salt=b"",
         sizes=(100, 3 * 1024 * 1024 + 77), fragmented=True),
)


def merkle(data, block_size, algorithm, salt):
    """Return the tree stored root level first and the root hash (fs-verity)."""
    name, digest_size, hash_block = ALGORITHMS[algorithm]
    padded = salt + bytes(-len(salt) % hash_block) if salt else b""

    def digest(block):
        return hashlib.new(name, padded + block).digest()

    if not data:
        return b"", bytes(digest_size)
    blocks = [data[offset:offset + block_size].ljust(block_size, b"\0")
              for offset in range(0, len(data), block_size)]
    levels = []
    while len(blocks) > 1:
        hashes = b"".join(digest(block) for block in blocks)
        blocks = [hashes[offset:offset + block_size].ljust(block_size, b"\0")
                  for offset in range(0, len(hashes), block_size)]
        levels.append(blocks)
    return b"".join(b"".join(level) for level in reversed(levels)), digest(blocks[0])


def descriptor(size, block_size, algorithm, salt, root, signature_size=0):
    return DESCRIPTOR.pack(VERITY_VERSION, algorithm, block_size.bit_length() - 1, len(salt),
                           signature_size, size, root.ljust(64, b"\0"), salt.ljust(32, b"\0"),
                           bytes(144))


def layout(data, fs_block, block_size, algorithm, salt, damage=None, signature=b""):
    """Return sparse extents (offset, bytes), the file digest and tree geometry. A
    built-in signature follows the descriptor; the digest excludes it."""
    tree, root = merkle(data, block_size, algorithm, salt)
    name = ALGORITHMS[algorithm][0]
    digest = hashlib.new(name, descriptor(len(data), block_size, algorithm, salt, root)).digest()
    if damage == "root":
        root = bytes([root[0] ^ 1]) + root[1:]
    record = descriptor(len(data), block_size, algorithm, salt, root, len(signature)) + signature
    if damage == "version":
        record = bytes([VERITY_VERSION + 1]) + record[1:]
    metadata = -(-len(data) // METADATA_ALIGNMENT) * METADATA_ALIGNMENT
    descriptor_position = -(-(metadata + len(tree)) // fs_block) * fs_block
    size_position = -(-(descriptor_position + len(record) + SIZE_FIELD.size) // fs_block) * \
        fs_block - SIZE_FIELD.size
    stored_size = len(record) if damage != "size" else size_position + fs_block
    pieces = [(0, bytearray(data)), (metadata, bytearray(tree)),
              (descriptor_position, bytearray(record)),
              (size_position, bytearray(SIZE_FIELD.pack(stored_size)))]
    if damage == "data":
        pieces[0][1][len(data) // 2] ^= 0x40
    elif damage == "tree":
        # Level zero is stored last; damage the hash of the first data block.
        data_blocks = -(-len(data) // block_size)
        level_zero_blocks = -(-data_blocks * ALGORITHMS[algorithm][1] // block_size)
        pieces[1][1][len(tree) - level_zero_blocks * block_size] ^= 0x01
    return pieces, digest.hex(), metadata


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--tools-root", type=Path)
    parser.add_argument("--profile", choices=[profile["name"] for profile in PROFILES],
                        help="Generate only this profile (default: all)")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    tools = resolve_tools(args.tools_root)
    reports = []
    for profile in PROFILES:
        if args.profile and profile["name"] != args.profile:
            continue
        fs_block = profile["block_size"]
        image = output / f"verity-{profile['name']}.img"
        manifest = output / f"verity-{profile['name']}.manifest"
        row = dict(profile=profile["name"], image=str(image), manifest=str(manifest),
                   commands=[], files=[], passed=False)
        reports.append(row)

        def run(command, row=row):
            result = subprocess.run([str(x) for x in command], capture_output=True, text=True,
                                    errors="backslashreplace", timeout=300)
            row["commands"].append(dict(command=[str(x) for x in command],
                                        status=result.returncode, stdout=result.stdout[-2000:],
                                        stderr=result.stderr[-2000:]))
            (output / "report.json").write_text(json.dumps(reports, indent=2) + "\n")
            result.check_returncode()
            return result.stdout

        features = EXPECTED_FEATURES | {"verity"} | profile.get("features", set())
        run([tools["mke2fs"], "-F", "-t", "ext4", "-b", fs_block, "-N", 256, "-I", 256, "-m", 0,
             "-O", "none," + ",".join(sorted(features)), "-U", UUID,
             "-E", "lazy_itable_init=0,nodiscard", image, IMAGE_BYTES // fs_block])
        generator = random.Random(profile["name"])
        cases = [("good", size, None) for size in profile["sizes"]]
        largest = max(profile["sizes"])
        cases += [(damage, largest, damage) for damage in ("data", "tree", "root", "version",
                                                            "size")]
        commands = []
        lines = []
        for index, (kind, size, damage) in enumerate(cases):
            data = bytearray(generator.randbytes(size))
            if profile.get("fragmented"):
                # debugfs leaves these zero blocks sparse; alternate holes force
                # checked mapping reads to cross external extent-leaf boundaries.
                for offset in range(fs_block, size, 2 * fs_block):
                    length = min(fs_block, size - offset)
                    data[offset:offset + length] = bytes(length)
            elif size > 3 * fs_block:
                # A sparse data run exercises holes covered by the tree.
                data[fs_block:3 * fs_block] = bytes(2 * fs_block)
            data = bytes(data)
            name = f"{kind}-{index}"
            pieces, digest, metadata = layout(data, fs_block, fs_block, profile["algorithm"],
                                              profile["salt"], damage)
            source = output / f"{profile['name']}-{name}.bin"
            with source.open("wb") as stream:
                for offset, payload in pieces:
                    stream.seek(offset)
                    stream.write(payload)
            commands += [f'write "{source}" {name}', f"sif {name} size {size}",
                         f"sif {name} flags {INODE_FLAGS_EXTENTS_VERITY:#x}"]
            failure = {"data": size // 2 // fs_block, "tree": 0}.get(damage, "all")
            lines.append(f"{kind} {name} {size} {hashlib.sha256(data).hexdigest()} "
                         f"{digest} {failure if damage else '-'}")
            row["files"].append(dict(name=name, kind=kind, size=size, metadata=metadata,
                                     file_digest=digest, sha256=hashlib.sha256(data).hexdigest()))
        script = output / f"verity-{profile['name']}.debugfs"
        script.write_text("\n".join(commands) + "\n")
        run([tools["debugfs"], "-w", "-f", script, image])
        for source in output.glob(f"{profile['name']}-*.bin"):
            source.unlink()
        run([tools["e2fsck"], "-fn", image])
        manifest.write_text(
            f"algorithm {profile['algorithm']} block {fs_block}\n" + "\n".join(lines) + "\n")
        row["passed"] = True
        (output / "report.json").write_text(json.dumps(reports, indent=2) + "\n")
        print(f"PASS verity {profile['name']}: {len(cases)} files", flush=True)


if __name__ == "__main__":
    main()
