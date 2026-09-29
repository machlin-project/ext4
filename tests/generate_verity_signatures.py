#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Sign fs-verity files with OpenSSL and have the core enable them with those signatures.

A self-signed RSA certificate signs the formatted digest of each file, computed
independently by generate_verity_fixtures.layout, as a detached PKCS#7 signature
without certificates or attributes, the form fsverity-utils produces. The core's
ext4-verity-enable-test --import stores the signatures on a copy of a verity image,
and check_verity_enable.py checks the result independently. Besides correctly signed
files, the image holds an unsigned verity file, one signed by a certificate the
keyring lacks and one whose signature is damaged, which a Linux keyring requiring
signatures must refuse. The output holds the signer's DER certificate for
tests/run_linux_verity.py --certificate."""
import argparse
import hashlib
import json
from pathlib import Path
import random
import shutil
import struct
import subprocess
import sys

from generate_verity_fixtures import ALGORITHMS, layout

FORMATTED_DIGEST = struct.Struct("<8sHH")
FORMATTED_MAGIC = b"FSVerity"
# name, kind, size, algorithm, Merkle block size, salt, signer
FILES = (
    ("signed-sha256", "good", 20000, 1, 4096, b"", "signer"),
    ("signed-sha512", "good", 70000, 2, 4096, bytes(range(32)), "signer"),
    ("signed-small-merkle", "good", 9000, 1, 1024, bytes(range(16)), "signer"),
    ("unsigned", "unsigned", 5000, 1, 4096, b"", None),
    ("other-signer", "badsig", 5000, 1, 4096, b"", "other"),
    ("damaged-signature", "badsig", 5000, 1, 4096, b"", "damaged"),
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tools-root", type=Path)
    parser.add_argument("--enable-test", required=True, type=Path,
                        help="ext4-verity-enable-test executable")
    parser.add_argument("--image", required=True, type=Path,
                        help="4 KiB volume with the verity feature")
    parser.add_argument("--openssl", default="openssl")
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    imports = output / "import"
    exports = output / "exports"
    imports.mkdir()
    exports.mkdir()
    commands = []

    def run(command):
        result = subprocess.run([str(x) for x in command], capture_output=True, text=True,
                                errors="backslashreplace", timeout=600)
        commands.append(dict(command=[str(x) for x in command], status=result.returncode,
                             stdout=result.stdout[-2000:], stderr=result.stderr[-2000:]))
        (output / "report.json").write_text(json.dumps(commands, indent=2) + "\n")
        result.check_returncode()
        return result.stdout

    for signer in ("signer", "other"):
        run([args.openssl, "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "3650",
             "-subj", f"/CN=machlin fs-verity {signer}", "-keyout", output / f"{signer}.key",
             "-out", output / f"{signer}.pem"])
    run([args.openssl, "x509", "-in", output / "signer.pem", "-outform", "DER",
         "-out", output / "signer.der"])
    generator = random.Random("fs-verity signatures")
    lines = []
    for name, kind, size, algorithm, merkle, salt, signer in FILES:
        data = generator.randbytes(size)
        (imports / f"{name}.data").write_bytes(data)
        _, digest, _ = layout(data, merkle, merkle, algorithm, salt)
        digest = bytes.fromhex(digest)
        message = FORMATTED_DIGEST.pack(FORMATTED_MAGIC, algorithm, len(digest)) + digest
        if ALGORITHMS[algorithm][1] != len(digest):
            raise RuntimeError(f"{name}: unexpected digest size")
        signature_name = "-"
        if signer is not None:
            key = "other" if signer == "other" else "signer"
            (imports / f"{name}.message").write_bytes(message)
            signature_name = f"{name}.sig"
            run([args.openssl, "smime", "-sign", "-binary", "-noattr", "-nocerts",
                 "-outform", "DER", "-md", "sha256", "-in", imports / f"{name}.message",
                 "-signer", output / f"{key}.pem", "-inkey", output / f"{key}.key",
                 "-out", imports / signature_name])
            if signer == "damaged":
                signature = bytearray((imports / signature_name).read_bytes())
                signature[-1] ^= 0x01
                (imports / signature_name).write_bytes(bytes(signature))
        lines.append(f"{kind} {name} {name}.data {algorithm} {merkle} "
                     f"{salt.hex() if salt else '-'} {signature_name}")
    (imports / "import.manifest").write_text("\n".join(lines) + "\n")
    run([args.enable_test, "--import", imports, "--export", exports, args.image])
    for signature in imports.glob("*.sig"):
        shutil.copyfile(signature, exports / signature.name)
    checker = Path(__file__).resolve().parent / "check_verity_enable.py"
    command = [sys.executable, checker, "--exports", exports, "--output", output / "independent"]
    if args.tools_root is not None:
        command += ["--tools-root", args.tools_root]
    print(run(command), end="", flush=True)
    print(f"PASS verity signatures: {len(FILES)} files, "
          f"{hashlib.sha256((output / 'signer.der').read_bytes()).hexdigest()[:16]} signer",
          flush=True)


if __name__ == "__main__":
    main()
