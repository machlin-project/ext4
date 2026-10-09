#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Independently verify exported sustained-operation images with e2fsprogs.

Objects are addressed by inode number, so encrypted directories need no plaintext
names. Their raw ciphertext entries must be those the exported no-key names carry.
Encrypted verity contents and metadata are decrypted with the deterministic test
key and verified; other encrypted contents and targets are counted without decryption.
Encryption, casefold and verity flags must match, the fscrypt context must be
present exactly on encrypted objects, and each verity file's digest, Merkle tree and
descriptor are recomputed from its expected contents."""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import re
import subprocess

from check_namespace import inode_fields, symlink_bytes
from check_rename import entries
from check_verity_enable import extents, read_file
from generate_fixtures import resolve_tools
from generate_verity_fixtures import layout

KIND_TYPES = {"file": "regular", "directory": "directory", "symlink": "symlink",
              "fifo": "FIFO", "device": "character"}
PERMISSION_BITS = 0o7777
ENCRYPT_FLAG = 0x800
CASEFOLD_FLAG = 0x40000000
VERITY_FLAG = 0x100000
# A version 2 fscrypt context, stored as attribute "c" in index 9.
CONTEXT_KEY = "c"
CONTEXT_BYTES = 40
# Linux's no-key names: two hash words, up to 149 ciphertext bytes, then the
# SHA-256 of the rest.
NOKEY_HASH_BYTES = 8
NOKEY_CIPHER_BYTES = 149
RAW_ENTRY = re.compile(r"^\s*(\d+)\s+[0-7]+\s+\(\d+\)\s+\d+\s+\d+\s+\d+\s+\S+ \S+ (.*)$")


def parse_manifest(path):
    root = None
    objects = []
    xattrs = {}
    features = dict(encrypted=set(), casefold=set(), verity={}, nokey={})
    for line in path.read_text().splitlines():
        fields = line.split(" ")
        if fields[0] == "root":
            root = fields[1]
        elif fields[0] == "xattr":
            number, key, value = int(fields[1]), fields[2], bytes.fromhex(fields[3])
            xattrs.setdefault(number, {})[key] = value
        elif fields[0] in ("encrypted", "casefold"):
            features[fields[0]].add(int(fields[1]))
        elif fields[0] == "verity":
            number, algorithm, block, salt, digest = fields[1:]
            features["verity"][int(number)] = dict(
                algorithm=int(algorithm), block=int(block),
                salt=b"" if salt == "-" else bytes.fromhex(salt), digest=digest)
        elif fields[0] == "nokey":
            directory, number, name = int(fields[1]), int(fields[2]), fields[3]
            features["nokey"].setdefault(directory, {})[nokey_body(name)] = number
        else:
            kind, number, links, mode, name = fields
            if kind not in KIND_TYPES or not name.startswith("/"):
                raise RuntimeError(f"Malformed manifest line: {line!r}")
            objects.append(dict(kind=kind, number=int(number), links=int(links),
                                mode=int(mode, 8), path=name))
    if root is None:
        raise RuntimeError("Manifest lacks the sustained root")
    return root, objects, xattrs, features


def nokey_body(name):
    """The ciphertext part of a no-key name, without its hash words."""
    decoded = base64.urlsafe_b64decode(name + "=" * (-len(name) % 4))
    if len(decoded) <= NOKEY_HASH_BYTES:
        raise RuntimeError(f"Malformed no-key name {name!r}")
    return decoded[NOKEY_HASH_BYTES:]


def cipher_body(cipher):
    if len(cipher) <= NOKEY_CIPHER_BYTES:
        return cipher
    return cipher[:NOKEY_CIPHER_BYTES] + hashlib.sha256(cipher[NOKEY_CIPHER_BYTES:]).digest()


def raw_entries(text):
    """Stored names of a raw long listing, whose bytes debugfs escapes as \\xNN."""
    result = {}
    for line in text.splitlines():
        # Unused records, such as an index node's, list inode zero without a date.
        if not line.strip() or line.split()[0] == "0":
            continue
        match = RAW_ENTRY.match(line)
        if match is None:
            raise RuntimeError(f"Malformed raw directory listing: {line!r}")
        text_name, name, index = match[2], bytearray(), 0
        while index < len(text_name):
            if text_name.startswith("\\x", index):
                name.append(int(text_name[index + 2:index + 4], 16))
                index += 4
            else:
                name.append(ord(text_name[index]))
                index += 1
        if bytes(name) in (b".", b".."):
            continue
        if bytes(name) in result:
            raise RuntimeError("Duplicate stored name in an encrypted directory")
        result[bytes(name)] = int(match[1])
    return result


def sustained_contents_key(context):
    """The sustained harness uses only v2 XTS/CTS, PAD32, filesystem-block IVs."""
    from cryptography.hazmat.primitives import hashes
    from cryptography.hazmat.primitives.kdf.hkdf import HKDF

    if len(context) != CONTEXT_BYTES or context[:8] != bytes((2, 1, 4, 3, 0, 0, 0, 0)):
        raise RuntimeError("Unsupported sustained fscrypt context")
    master = bytes((i * 7 + 3) & 255 for i in range(64))
    identifier = HKDF(algorithm=hashes.SHA512(), length=16, salt=bytes(64),
                      info=b"fscrypt\0\x01").derive(master)
    if context[8:24] != identifier:
        raise RuntimeError("Sustained fscrypt master key identifier differs")
    return HKDF(algorithm=hashes.SHA512(), length=64, salt=bytes(64),
                info=b"fscrypt\0\x02" + context[24:40]).derive(master)


def read_encrypted_file(image, mapping, block_size, offset, length, key, required):
    """Decrypt complete filesystem blocks with absolute file-logical XTS IVs.

    Unmapped/unwritten data reads as plaintext zeros. Metadata must be written;
    treating missing ciphertext as a zero plaintext block would hide corruption.
    """
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

    result = bytearray()
    with image.open("rb") as stream:
        while length:
            logical, within = divmod(offset, block_size)
            count = min(length, block_size - within)
            runs = [run for run in mapping if run[0] <= logical < run[0] + run[2]]
            if len(runs) > 1:
                raise RuntimeError("Overlapping encrypted file extents")
            if not runs or runs[0][3]:
                if required:
                    raise RuntimeError("Missing or unwritten encrypted verity metadata")
                plain = bytes(block_size)
            else:
                first, physical, _, _ = runs[0]
                stream.seek((physical + logical - first) * block_size)
                cipher = stream.read(block_size)
                if len(cipher) != block_size:
                    raise RuntimeError("Short encrypted filesystem block")
                decoder = Cipher(algorithms.AES(key),
                                 modes.XTS(logical.to_bytes(16, "little"))).decryptor()
                plain = decoder.update(cipher) + decoder.finalize()
            result.extend(plain[within:within + count])
            offset += count
            length -= count
    return bytes(result)


def check_verity(image, number, data, verity, block_size, tools, run, key=None):
    pieces, digest, _ = layout(data, block_size, verity["block"], verity["algorithm"],
                               verity["salt"])
    if digest != verity["digest"]:
        raise RuntimeError(f"Verity digest of inode {number} differs")
    mapping = extents(run([tools["debugfs"], "-R", f"dump_extents <{number}>", image]))
    if key is not None and read_encrypted_file(image, mapping, block_size, 0, len(data),
                                               key, False) != data:
        raise RuntimeError(f"Encrypted contents of verity inode {number} differ")
    for offset, payload in pieces[1:]:
        actual = read_file(image, mapping, block_size, offset, len(payload)) if key is None else \
            read_encrypted_file(image, mapping, block_size, offset, len(payload), key, True)
        if actual != bytes(payload):
            raise RuntimeError(f"Verity metadata of inode {number} differs at {offset}")


def check_listings(image, root, expected_children, numbers, features, tools, run):
    encrypted_names = 0
    for parent, names in expected_children.items():
        number = numbers.get(parent)
        if number is None and parent:
            raise RuntimeError(f"Manifest lacks directory {parent}")
        where = f'"/{root}"' if number is None else f"<{number}>"
        if number in features["encrypted"]:
            stored = raw_entries(run([tools["debugfs"], "-R", f"ls -l -r {where}", image]))
            actual = {cipher_body(cipher): child for cipher, child in stored.items()}
            nokey = features["nokey"].get(number, {})
            if actual != nokey or sorted(nokey.values()) != sorted(names.values()):
                raise RuntimeError(f"Encrypted directory {parent} differs from its no-key names")
            encrypted_names += len(names)
            continue
        actual = entries(run([tools["debugfs"], "-R", f"ls -p {where}", image]))
        actual = {name: child for name, child in actual.items() if name not in (".", "..")}
        if actual != names:
            raise RuntimeError(f"Directory /{root}{parent} differs: "
                               f"missing {sorted(set(names) - set(actual))[:4]}, "
                               f"extra {sorted(set(actual) - set(names))[:4]}")
    return encrypted_names


def check_attributes(image, item, where, values, encrypted, output, tools, run):
    listed = dict(re.findall(r'^\s+([^\s]+)\s+\((\d+)\)',
                             run([tools["debugfs"], "-R", f"ea_list {where}", image]), re.M))
    context = listed.pop(CONTEXT_KEY, None)
    if (context is not None) != encrypted or (context is not None and int(context) != CONTEXT_BYTES):
        raise RuntimeError(f"fscrypt context differs for {item['path']}")
    if set(listed) != set(values):
        raise RuntimeError(f"Attribute keys differ for {item['path']}")
    for index, (key, value) in enumerate(values.items()):
        dump = output / f"attribute-{item['number']}-{index}"
        run([tools["debugfs"], "-R", f'ea_get -r -f "{dump}" {where} "{key}"', image])
        if dump.read_bytes() != value:
            raise RuntimeError(f"Attribute {key} differs for {item['path']}")
        dump.unlink()


def check_export(image, manifest, directory, output, tools, run,
                 require_encrypted_verity=False):
    root, objects, xattrs, features = parse_manifest(manifest)
    encrypted = features["encrypted"]
    run([tools["e2fsck"], "-fn", image])
    info = run([tools["dumpe2fs"], "-h", image])
    match = re.search(r"^Block size:\s+(\d+)", info, re.M)
    if match is None:
        raise RuntimeError("Missing independently decoded block size")
    block_size = int(match[1])
    expected_children = {"": {}}
    numbers = {"": None}
    for item in objects:
        parent, _, name = item["path"].rpartition("/")
        if expected_children.setdefault(parent, {}).setdefault(name, item["number"]) != \
                item["number"]:
            raise RuntimeError(f"Manifest names {item['path']} twice")
        if item["kind"] == "directory":
            expected_children.setdefault(item["path"], {})
            numbers[item["path"]] = item["number"]
    counts = dict(encrypted_names=check_listings(image, root, expected_children, numbers,
                                                 features, tools, run),
                  encrypted_contents=0, encrypted_verity=0, verity=0, casefold=0)
    checked = set()
    for item in objects:
        number = item["number"]
        where = f"<{number}>"
        inode = inode_fields(run([tools["debugfs"], "-R", f"stat {where}", image]))
        if inode is None or inode["inode"] != number:
            raise RuntimeError(f"Inode identity differs for {item['path']}")
        if inode["type"] != KIND_TYPES[item["kind"]]:
            raise RuntimeError(f"Inode type differs for {item['path']}")
        if inode["links"] != item["links"] or inode["mode"] & PERMISSION_BITS != item["mode"]:
            raise RuntimeError(f"Link count or permissions differ for {item['path']}")
        if bool(inode["flags"] & ENCRYPT_FLAG) != (number in encrypted) or \
                bool(inode["flags"] & CASEFOLD_FLAG) != (number in features["casefold"]) or \
                bool(inode["flags"] & VERITY_FLAG) != (number in features["verity"]):
            raise RuntimeError(f"Encryption, casefold or verity flag differs for {item['path']}")
        if number in checked:
            continue
        checked.add(number)
        counts["casefold"] += number in features["casefold"]
        data = directory / f"object-{number}.data"
        if item["kind"] == "file" and number in encrypted and number in features["verity"]:
            dump = output / f"context-{number}"
            run([tools["debugfs"], "-R", f'ea_get -r -f "{dump}" {where} "{CONTEXT_KEY}"', image])
            key = sustained_contents_key(dump.read_bytes())
            dump.unlink()
            expected = data.read_bytes()
            if inode["size"] != len(expected):
                raise RuntimeError(f"Encrypted verity size differs for {item['path']}")
            check_verity(image, number, expected, features["verity"][number], block_size,
                         tools, run, key)
            counts["verity"] += 1
            counts["encrypted_verity"] += 1
        elif item["kind"] in ("file", "symlink") and number in encrypted:
            counts["encrypted_contents"] += 1
        elif item["kind"] == "file":
            dump = output / f"file-{number}"
            run([tools["debugfs"], "-R", f'dump {where} "{dump}"', image])
            actual = dump.read_bytes()
            dump.unlink()
            if actual != data.read_bytes():
                raise RuntimeError(f"File contents differ for {item['path']}")
            if number in features["verity"]:
                check_verity(image, number, actual, features["verity"][number], block_size,
                             tools, run)
                counts["verity"] += 1
        elif item["kind"] == "symlink":
            target = symlink_bytes(image, where, inode, block_size, tools["debugfs"], run)
            if target != data.read_bytes():
                raise RuntimeError(f"Symlink target differs for {item['path']}")
        check_attributes(image, item, where, xattrs.get(number, {}), number in encrypted, output,
                         tools, run)
    if counts["verity"] != len(features["verity"]):
        raise RuntimeError("A verity file was not verified")
    if require_encrypted_verity and counts["encrypted_verity"] == 0:
        raise RuntimeError("Required encrypted verity coverage is empty for this export")
    return dict(objects=len(objects), directories=len(expected_children),
                xattrs=sum(len(values) for values in xattrs.values()), **counts)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--tools-root", type=Path, required=True)
    parser.add_argument("--exports", type=Path, required=True, action="append",
                        help="directory holding one exported image and its manifest")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--require-encrypted-verity", action="store_true",
                        help="require a decrypted and verified verity inode in every export")
    args = parser.parse_args()
    tools = resolve_tools(args.tools_root)
    args.output.mkdir(parents=True, exist_ok=False)
    rows = []

    for directory in args.exports:
        images = sorted(directory.glob("sustained-*.img"))
        if len(images) != 1:
            raise RuntimeError(f"Expected one exported image in {directory}")
        image = images[0]
        row = dict(export=str(directory), image=image.name, commands=[], passed=False)
        rows.append(row)
        before = hashlib.sha256(image.read_bytes()).hexdigest()

        def run(command, row=row):
            command = [str(part) for part in command]
            done = subprocess.run(command, capture_output=True, text=True, errors="replace",
                                  check=False)
            row["commands"].append(dict(command=command, status=done.returncode,
                                        stderr=done.stderr[-2000:]))
            if done.returncode != 0:
                raise RuntimeError(f"Command failed ({done.returncode}): {command}\n"
                                   f"{done.stdout[-2000:]}{done.stderr[-2000:]}")
            return done.stdout

        work = args.output / directory.name
        work.mkdir()
        row.update(check_export(image, directory / "manifest.txt", directory, work, tools, run,
                                args.require_encrypted_verity))
        if hashlib.sha256(image.read_bytes()).hexdigest() != before:
            raise RuntimeError("Independent verification changed the exported image")
        row["passed"] = True
        (args.output / "report.json").write_text(json.dumps(rows, indent=2) + "\n")
        print(f"PASS sustained export {directory.name}: strict e2fsck, {row['objects']} names, "
              f"{row['xattrs']} attributes, {row['encrypted_names']} encrypted names, "
              f"{row['encrypted_contents']} encrypted contents not decrypted, "
              f"{row['verity']} verity files ({row['encrypted_verity']} decrypted), "
              f"{row['casefold']} casefolded directories",
              flush=True)


if __name__ == "__main__":
    main()
