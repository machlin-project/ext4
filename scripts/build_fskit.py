#!/usr/bin/env python3
"""Build the macOS FSKit app, unsigned unless a personal signing team is supplied."""

import argparse
import os
from pathlib import Path
import pwd
import subprocess


ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--team", help="Personal Apple Developer team for signing")
    parser.add_argument("--provision", action="store_true", help="Allow Xcode to fetch signing profiles")
    parser.add_argument("--app-profile", help="App development profile name or UUID (requires --extension-profile)")
    parser.add_argument("--extension-profile", help="Extension development profile name or UUID (requires --app-profile)")
    parser.add_argument("--clean", action="store_true", help="Clean products before building, for example after changing profiles")
    parser.add_argument("--derived-data", type=Path, default=ROOT / "artifacts/fskit/DerivedData")
    args = parser.parse_args()
    if args.provision and not args.team:
        parser.error("--provision requires --team")
    manual = bool(args.app_profile or args.extension_profile)
    if manual and not (args.team and args.app_profile and args.extension_profile):
        parser.error("manual signing requires --team, --app-profile and --extension-profile")
    derived_data = args.derived_data.resolve()
    project = ROOT / "adapters/fskit"
    environment = os.environ.copy()
    username = pwd.getpwuid(os.getuid()).pw_name
    environment.setdefault("USER", username)
    environment.setdefault("LOGNAME", username)
    for name in ("CFLAGS", "CPPFLAGS", "CXXFLAGS", "LDFLAGS", "CC", "CXX", "SDKROOT"):
        environment.pop(name, None)
    clang = subprocess.check_output(["xcrun", "--find", "clang"], text=True).strip()
    subprocess.run(
        ["xcodegen", "generate", "--spec", str(project / "project.yml"), "--project", str(project)],
        check=True, cwd=ROOT, env=environment,
    )
    command = [
        "xcodebuild", "-project", str(project / "Ext4FSKit.xcodeproj"),
        "-scheme", "Ext4FSKitApp", "-configuration", "Debug", "-sdk", "macosx",
        "-destination", "generic/platform=macOS", "-derivedDataPath", str(derived_data),
        "-jobs", "4", "CLANG_ENABLE_EXPLICIT_MODULES=NO", f"CC={clang}",
    ]
    if args.team:
        command.extend([
            f"CODE_SIGN_STYLE={'Manual' if manual else 'Automatic'}",
            "CODE_SIGN_IDENTITY=Apple Development",
            f"DEVELOPMENT_TEAM={args.team}",
        ])
        if manual:
            command.extend([
                f"EXT4_APP_PROFILE={args.app_profile}",
                f"EXT4_EXTENSION_PROFILE={args.extension_profile}",
            ])
        if args.provision:
            command.append("-allowProvisioningUpdates")
    else:
        command.append("CODE_SIGNING_ALLOWED=NO")
    if args.clean:
        command.append("clean")
    command.append("build")
    subprocess.run(command, check=True, cwd=ROOT, env=environment)
    if args.team:
        app = derived_data / "Build/Products/Debug/Machlin ext4.app"
        subprocess.run(
            ["codesign", "--verify", "--deep", "--strict", "--all-architectures", str(app)],
            check=True, cwd=ROOT, env=environment,
        )


if __name__ == "__main__":
    main()
