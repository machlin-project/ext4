#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Author casefolded directories with e2fsprogs for core lookup and mutation tests."""
import argparse
import json
from pathlib import Path
import subprocess
import unicodedata

from generate_fixtures import EXPECTED_FEATURES, UUID, resolve_tools

IMAGE_BYTES = 32 * 1024 * 1024
CASEFOLD_FLAGS = 0x40080000
BULK_NAMES = 400
PROFILES = (
    dict(name="4k-strict", block_size=4096, strict=True),
    dict(name="1k-relaxed", block_size=1024, strict=False),
)
# Stored name, then lookups that must resolve to it under utf8-12.1 casefolding.
EQUIVALENT = (
    ("Straße", ("STRASSE", "strasse", "STRAẞE", "Strasse")),
    ("ÅNGSTRÖM", ("ångström", unicodedata.normalize("NFD", "ångström"), "ÅNGSTRÖM")),
    ("ﬁle", ("FILE", "file", "File")),
    ("café", ("CAFÉ", unicodedata.normalize("NFD", "Café"), "ca​fé")),
    ("Ωmega", ("ωMEGA", "Ωmega")),
    ("İstanbul", ("i̇stanbul", "İSTANBUL")),
)
ABSENT = ("cafe", "strase", "fil", "omega", "istanbul")
OPAQUE = b"opaque-\xff\xfe-Name"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--tools-root", type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    tools = resolve_tools(args.tools_root)
    payload = output / "payload"
    payload.write_bytes(b"casefolded contents\n")
    reports = []
    for profile in PROFILES:
        image = output / f"casefold-{profile['name']}.img"
        manifest = output / f"casefold-{profile['name']}.manifest"
        row = dict(profile=profile["name"], image=str(image), manifest=str(manifest),
                   strict=profile["strict"], commands=[], passed=False)
        reports.append(row)

        def run(command, row=row, stdin=None):
            result = subprocess.run([str(x) for x in command], capture_output=True,
                                    input=stdin, timeout=300)
            row["commands"].append(dict(command=[str(x) for x in command],
                                        status=result.returncode,
                                        stdout=result.stdout[-2000:].decode(errors="replace"),
                                        stderr=result.stderr[-2000:].decode(errors="replace")))
            (output / "report.json").write_text(json.dumps(reports, indent=2) + "\n")
            result.check_returncode()
            return result.stdout

        features = EXPECTED_FEATURES | {"casefold"}
        encoding = "encoding=utf8" + (",encoding_flags=strict" if profile["strict"] else "")
        run([tools["mke2fs"], "-F", "-t", "ext4", "-b", profile["block_size"], "-N", 2048,
             "-I", 256, "-m", 0, "-O", "none," + ",".join(sorted(features)), "-U", UUID,
             "-E", f"{encoding},lazy_itable_init=0,nodiscard", image,
             IMAGE_BYTES // profile["block_size"]])
        names = [stored.encode() for stored, _ in EQUIVALENT]
        small = list(names)
        names += [f"Bulk-{index:04d}-ÉTÉ".encode() for index in range(BULK_NAMES)]
        # e2fsck -D cannot rebuild an index holding a non-UTF-8 name, so the opaque
        # name stays in the linear directory.
        if not profile["strict"]:
            small.append(OPAQUE)
        script = [b"mkdir cf", f"sif cf flags {CASEFOLD_FLAGS:#x}".encode(), b"mkdir small",
                  f"sif small flags {CASEFOLD_FLAGS:#x}".encode()]
        script += [b'write "' + str(payload).encode() + b'" cf/' + name for name in names]
        script += [b'write "' + str(payload).encode() + b'" small/' + name for name in small]
        commands = output / f"casefold-{profile['name']}.debugfs"
        commands.write_bytes(b"\n".join(script) + b"\n")
        run([tools["debugfs"], "-w", "-f", commands, image])
        # Rebuild directories so every index uses e2fsprogs' casefolded hashes.
        run([tools["e2fsck"], "-fyD", image])
        run([tools["e2fsck"], "-fn", image])
        lines = [f"strict {int(profile['strict'])}"]
        for stored, lookups in EQUIVALENT:
            for lookup in lookups + (stored,):
                lines.append(f"match {lookup.encode().hex()} {stored.encode().hex()}")
        for absent in ABSENT:
            lines.append(f"absent {absent.encode().hex()}")
        lines += [f"bulk {BULK_NAMES}"]
        if not profile["strict"]:
            lines.append(f"match {OPAQUE.hex()} {OPAQUE.hex()}")
            lines.append(f"absent {OPAQUE.upper().hex()}")
        manifest.write_text("\n".join(lines) + "\n")
        row["passed"] = True
        (output / "report.json").write_text(json.dumps(reports, indent=2) + "\n")
        print(f"PASS casefold {profile['name']}: {len(names)} names", flush=True)


if __name__ == "__main__":
    main()
