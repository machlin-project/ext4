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
    parser.add_argument("--suite", choices=("all", "resource", "control", "volume"), default="all")
    args = parser.parse_args()
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
                   "Ext4VolumeIO.m", "Ext4VolumeControl.m"],
    }
    for name, sources in suites.items():
        if args.suite != "all" and name != args.suite:
            continue
        binary = args.output / f"fskit-{name}"
        command = [
            clang, "-isysroot", sdk, "-fobjc-arc", "-fblocks", "-Wall", "-Wextra", "-Werror",
            "-Wdeclaration-after-statement", "-g", "-O1",
            "-fsanitize=address,undefined", "-mmacosx-version-min=26.4",
            "-I", str(ROOT / "include"), "-framework", "Foundation", "-framework", "FSKit",
            str(ROOT / f"tests/fskit_{name}.m"),
            *(str(adapter / source) for source in sources),
            str(core), "-o", str(binary),
        ]
        subprocess.run(command, check=True)
        arguments = [str(args.image.resolve())] if name == "volume" else []
        subprocess.run([str(binary.resolve()), *arguments], check=True, timeout=60)


if __name__ == "__main__":
    main()
