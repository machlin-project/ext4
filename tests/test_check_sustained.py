#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Exercise encrypted sustained verification on independent file forks, not volumes."""
import hashlib
import hmac
from pathlib import Path
import sys
import tempfile

from check_sustained import check_export, check_verity, sustained_contents_key


def hkdf(info, size):
    """Reference extract/expand, separate from the checker's cryptography HKDF."""
    master = bytes((i * 7 + 3) & 255 for i in range(64))
    prk = hmac.new(bytes(64), master, hashlib.sha512).digest()
    result, previous = b"", b""
    for counter in range(1, (size + 63) // 64 + 1):
        previous = hmac.new(prk, previous + info + bytes((counter,)), hashlib.sha512).digest()
        result += previous
    return result[:size]


def rejects(action):
    try:
        action()
    except RuntimeError:
        return
    raise AssertionError("Corrupt encrypted verity input was accepted")


def check_required_coverage(directory):
    """A mocked empty export tests the opt-in guard, not filesystem acceptance."""
    manifest = directory / "empty-manifest.txt"
    manifest.write_text("root sustained\n")
    tools = {name: name for name in ("debugfs", "dumpe2fs", "e2fsck")}

    def run(command):
        if command[0] == "dumpe2fs":
            return "Block size: 4096\n"
        return ""

    def verify(required):
        return check_export(directory / "unused.img", manifest, directory, directory,
                            tools, run, require_encrypted_verity=required)

    assert verify(False)["encrypted_verity"] == 0
    try:
        verify(True)
    except RuntimeError as error:
        assert str(error) == "Required encrypted verity coverage is empty for this export"
    else:
        raise AssertionError("Required encrypted verity coverage was silently skipped")
    print("PASS per-export encrypted verity coverage guard (mocked empty export)")


def main():
    fixtures = Path(sys.argv[1])
    context = bytes((2, 1, 4, 3, 0, 0, 0, 0)) + hkdf(b"fscrypt\0\x01", 16) + bytes(range(16))
    key = sustained_contents_key(context)
    assert key == hkdf(b"fscrypt\0\x02" + bytes(range(16)), 64)
    for byte in (0, 1, 2, 3, 4, 5, 8, 23):
        damaged = bytearray(context)
        damaged[byte] ^= 0x80
        rejects(lambda: sustained_contents_key(damaged))
    rejects(lambda: sustained_contents_key(context[:-1]))
    count = 0
    with tempfile.TemporaryDirectory(prefix="ext4-sustained-oracle-") as temporary:
        check_required_coverage(Path(temporary))
        image = Path(temporary) / "fork.img"
        for name in (fixtures / "manifest").read_text().splitlines():
            fields, digest = (fixtures / f"{name}.meta").read_text().splitlines()
            block, merkle, size, stored, tree, descriptor, algorithm, version, sparse, signed = \
                map(int, fields.split())
            if version != 2 or signed:
                continue  # Sustained uses only v2 policies and unsigned descriptors.
            plain = (fixtures / f"{name}.plain").read_bytes()
            cipher = (fixtures / f"{name}.cipher").read_bytes()
            assert len(plain) == size and len(cipher) == stored
            original = bytes(block) + cipher
            image.write_bytes(original)
            mapping = [(logical, logical + 1, 1, False) for logical in range(stored // block)]
            verity = dict(block=merkle, algorithm=algorithm, salt=bytes(range(16)), digest=digest)

            def run(command):
                assert "dump_extents <17>" in command
                return "\n".join(f"0/0 1/1 {first} - {first + length - 1} "
                                 f"{physical} - {physical + length - 1} {length} "
                                 f"{'Uninit' if unwritten else ''}"
                                 for first, physical, length, unwritten in mapping)

            def verify(selected_key=key, selected_verity=verity):
                check_verity(image, 17, plain, selected_verity, block,
                             {"debugfs": "fixture-stub"}, run, selected_key)

            verify()
            wrong = bytes((key[0] ^ 1,)) + key[1:]
            rejects(lambda: verify(wrong))
            rejects(lambda: verify(selected_verity=dict(verity, digest="00" * (len(digest) // 2))))
            for offset in [descriptor, stored - 1] + ([0] if size else []) + \
                    ([tree] if descriptor > tree else []):
                damaged = bytearray(original)
                damaged[block + offset] ^= 0x40
                image.write_bytes(damaged)
                rejects(verify)
                image.write_bytes(original)
            metadata = {descriptor // block}
            if descriptor > tree:
                metadata.add(tree // block)
            for logical in sorted(metadata):
                saved = mapping[logical]
                mapping.remove(saved)
                rejects(verify)
                mapping.insert(logical, (*saved[:3], True))
                rejects(verify)
                mapping[logical] = saved
            mapping.append(mapping[descriptor // block])
            rejects(verify)
            mapping.pop()
            if sparse:
                saved_data = mapping.pop(1)
                verify()  # Sparse data is zero plaintext, unlike required metadata.
                mapping.insert(1, (*saved_data[:3], True))
                verify()
                mapping[1] = saved_data
            image.write_bytes(original[:-1])
            rejects(verify)
            image.write_bytes(original)
            count += 1
            print(f"PASS sustained encrypted verity oracle {name}")
    assert count == 21, count
    print(f"PASS {count} synthetic forks; context, wrong-key, corruption, holes, unwritten, short-I/O")


if __name__ == "__main__":
    main()
