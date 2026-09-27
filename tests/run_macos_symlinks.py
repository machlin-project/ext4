#!/usr/bin/env python3
"""Check readlink exports in an already prepared, identified Machlin lab macOS VM."""

import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import plistlib
import re
import shutil
import subprocess

from check_orphans import digest

MODULE = "org.machlin.ext4.kext"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lab", type=Path, required=True)
    parser.add_argument("--vm", required=True)
    parser.add_argument("--products", type=Path, required=True)
    parser.add_argument("--guest-directory", required=True)
    parser.add_argument("--share", required=True)
    parser.add_argument("--expected-session", required=True)
    parser.add_argument("--expected-module-uuid", required=True)
    parser.add_argument("--report", type=Path, action="append", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    lab = args.lab.resolve()
    products = args.products.resolve()
    output = args.output.resolve()
    if Path.cwd().resolve() != lab:
        parser.error("run from the explicit Machlin lab working directory")
    relative = output.relative_to(products)
    output.mkdir(parents=True, exist_ok=False)
    images = output / "images"
    images.mkdir()
    identity = json.loads((products / "kc-identity.json").read_text())
    if digest(products / identity["kc"]) != identity["sha256"] or digest(
            products / "MachlinExt4.kext/Contents/MacOS/machlin_ext4") != identity["ext4_executable_sha256"]:
        raise RuntimeError("Prepared kernel collection or module changed")
    cases = []
    for report in args.report:
        records = json.loads(report.read_text())
        if not records or not all(record.get("passed") for record in records):
            raise RuntimeError("Native inputs require complete independent checks")
        cases += [record for record in records if Path(record["image"]).name.startswith("symlinks-")]
    if not cases or len({Path(case["image"]).name for case in cases}) != len(cases):
        raise RuntimeError("Missing or duplicate native symlink cases")
    record = {"vm": args.vm, "kc_sha256": identity["sha256"], "cases": [], "commands": [],
              "module_uuid": args.expected_module_uuid, "result": "FAIL"}

    def save():
        (output / "report.json").write_text(json.dumps(record, indent=2) + "\n")

    def guest(*command):
        result = subprocess.run([str(lab / "scripts/tart.sh"), "exec", args.vm, *command],
                                cwd=lab, capture_output=True, timeout=120)
        record["commands"].append({"command": list(command), "status": result.returncode,
                                   "stdout": result.stdout.decode(errors="backslashreplace"),
                                   "stderr": result.stderr.decode(errors="backslashreplace")})
        save()
        result.check_returncode()
        return result.stdout

    def identity_checks():
        kernel = guest("/usr/sbin/sysctl", "-n", "kern.uuid").decode().strip()
        session = guest("/usr/sbin/sysctl", "-n", "kern.bootsessionuuid").decode().strip()
        module = guest("/usr/sbin/kextstat", "-l", "-b", MODULE).decode()
        if (kernel.upper() not in [value.upper() for value in identity["kernel_uuids"]] or
                session != args.expected_session):
            raise RuntimeError("Guest kernel or boot session does not match the prepared test")
        if MODULE not in module or args.expected_module_uuid.upper() not in module.upper():
            raise RuntimeError("Expected filesystem module is not loaded")
        record.update(kernel_uuid=kernel, boot_session=session, loaded_module=module)

    mountpoint = args.guest_directory + "/symlink-mount"
    try:
        identity_checks()
        record["os"] = guest("/usr/bin/sw_vers").decode()
        record["kernel"] = guest("/usr/bin/uname", "-a").decode().strip()
        record["uid"] = int(guest("/usr/bin/id", "-u").decode())
        if record["uid"] == 0:
            raise RuntimeError("Run mounted probes as the ordinary guest user")
        for binary, field in (("ext4-mounted-symlink-test", "mounted_probe_sha256"),
                              ("mount_machlin_ext4", "mount_helper_sha256")):
            actual = guest("/usr/bin/shasum", "-a", "256",
                           args.guest_directory + "/" + binary).decode().split()[0]
            if actual != identity[field] or digest(products / binary) != actual:
                raise RuntimeError(f"Prepared native binary changed: {binary}")
        guest("/usr/bin/sudo", "-n", "/bin/mkdir", "-p", mountpoint)
        for case in cases:
            source = Path(case["image"])
            expected = case["input_sha256"]
            if digest(source) != expected:
                raise RuntimeError("Independently checked symlink source changed")
            target = images / source.name
            shutil.copyfile(source, target)
            if digest(target) != expected:
                raise RuntimeError("Native fixture copy changed")
            shared = f"/Volumes/My Shared Files/{args.share}/{relative}/images/{source.name}"
            attached = plistlib.loads(guest("/usr/bin/hdiutil", "attach", "-nomount", "-readonly",
                                            "-imagekey", "diskimage-class=CRawDiskImage", "-plist", shared))
            devices = [entry["dev-entry"] for entry in attached["system-entities"] if "dev-entry" in entry]
            if len(devices) != 1 or not re.fullmatch(r"/dev/disk[0-9]+", devices[0]):
                raise RuntimeError(f"Unexpected attached raw device: {devices}")
            device = devices[0]
            item = {"image": str(source), "sha256": expected, "device": device,
                    "block_size": case["accounting"]["Block size"], "result": "FAIL"}
            record["cases"].append(item)
            mounted = False
            try:
                info = plistlib.loads(guest("/usr/sbin/diskutil", "info", "-plist", device))
                if (not info["WholeDisk"] or info["TotalSize"] != source.stat().st_size or
                        info.get("MountPoint") or info["WritableMedia"]):
                    raise RuntimeError("Guest device is not the expected unmounted read-only image")
                raw = "/dev/r" + device.removeprefix("/dev/")
                if guest("/usr/bin/sudo", "-n", "/usr/bin/shasum", "-a", "256", raw).decode().split()[0] != expected:
                    raise RuntimeError("Guest raw device differs from the checked source")
                guest("/usr/bin/sudo", "-n", args.guest_directory + "/mount_machlin_ext4", device, mountpoint)
                mounted = True
                item["probe"] = guest(args.guest_directory + "/ext4-mounted-symlink-test",
                                      mountpoint, str(item["block_size"])).decode()
                if "PASS mounted symbolic links:" not in item["probe"]:
                    raise RuntimeError("Missing native readlink success evidence")
                guest("/usr/bin/sudo", "-n", "/sbin/umount", mountpoint)
                mounted = False
                if guest("/usr/bin/sudo", "-n", "/usr/bin/shasum", "-a", "256", raw).decode().split()[0] != expected:
                    raise RuntimeError("Read-only mounted operations changed the raw device")
            finally:
                if mounted:
                    guest("/usr/bin/sudo", "-n", "/sbin/umount", mountpoint)
                guest("/usr/bin/hdiutil", "detach", device)
            if digest(source) != expected or digest(target) != expected:
                raise RuntimeError("Native checks changed a protected source or fixture copy")
            item["result"] = "PASS"
            save()
            print(f"PASS {source.name}: identified native kext, concurrent readlink, unchanged media", flush=True)
        identity_checks()
        record["result"] = "PASS"
    except Exception as error:
        record["error"] = str(error)
        raise
    finally:
        record["checked_at"] = datetime.now(timezone.utc).isoformat()
        save()


if __name__ == "__main__":
    main()
