#!/usr/bin/env python3
"""Run unsigned native adapter tests without mounting or registering an extension."""

import argparse
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--derived-data", type=Path, required=True)
    parser.add_argument("--image", type=Path, required=True,
                        help="Clean fixture authored by tests/generate_fixtures.py")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--suite", choices=("all", "resource", "control", "volume", "crypto", "keystore"), default="all")
    parser.add_argument("--encrypted-image", type=Path)
    parser.add_argument("--encrypted-manifest", type=Path)
    args = parser.parse_args()
    if (args.encrypted_image is None) != (args.encrypted_manifest is None):
        parser.error("--encrypted-image and --encrypted-manifest must be supplied together")
    args.output.mkdir(parents=True, exist_ok=True)
    core = args.derived_data / "Build/Products/Debug/libExt4Core.a"
    if not core.is_file():
        parser.error("Build the unsigned FSKit app first; libExt4Core.a is missing")
    clang = subprocess.check_output(["xcrun", "--find", "clang"], text=True).strip()
    sdk = subprocess.check_output(["xcrun", "--sdk", "macosx", "--show-sdk-path"], text=True).strip()
    adapter = ROOT / "adapters/fskit"
    suites = {
        "resource": ["Ext4ResourceIO.m"],
        "control": ["Ext4Control.m"],
        "volume": ["Ext4ResourceIO.m", "Ext4Support.m", "Ext4Control.m", "Ext4Volume.m",
                   "Ext4VolumeIO.m", "Ext4VolumeControl.m", "Ext4Crypto.m"],
        "crypto": ["Ext4Crypto.m"],
        "keystore": ["Ext4Crypto.m", "Ext4KeyStore.m", "Ext4Control.m"],
    }
    for name, sources in suites.items():
        if args.suite != "all" and name != args.suite:
            continue
        binary = args.output / f"fskit-{name}"
        overrides = []
        if name == "keystore":
            overrides = ["-DSecItemCopyMatching=Ext4TestKeychainCopy",
                         "-DSecItemAdd=Ext4TestKeychainAdd", "-DSecItemDelete=Ext4TestKeychainDelete",
                         "-framework", "Security", "-framework", "LocalAuthentication"]
        command = [
            clang, "-isysroot", sdk, "-fobjc-arc", "-fblocks", "-Wall", "-Wextra", "-Werror",
            "-Wdeclaration-after-statement", "-g", "-O1",
            "-fsanitize=address,undefined", "-mmacosx-version-min=26.4",
            "-I", str(ROOT / "include"), "-I", str(ROOT / "core"), "-framework", "Foundation", "-framework", "FSKit",
            *overrides,
            str(ROOT / f"tests/fskit_{name}.m"),
            *(str(adapter / source) for source in sources),
            str(core), "-o", str(binary),
        ]
        subprocess.run(command, check=True)
        arguments = [str(args.image.resolve())] if name == "volume" else []
        if name == "volume" and args.encrypted_image is not None:
            arguments.extend([str(args.encrypted_image.resolve()), str(args.encrypted_manifest.resolve())])
        subprocess.run([str(binary.resolve()), *arguments], check=True, timeout=60)


if __name__ == "__main__":
    main()
