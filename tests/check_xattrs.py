#!/usr/bin/env python3
"""Independently inspect xattr reader exports without repairing any image."""

import argparse
import json
from pathlib import Path
import re
import subprocess

from check_namespace import inode_fields
from check_orphans import accounting, digest
from generate_fixtures import resolve_tools

PREFIXES = {1: "user.", 2: "system.posix_acl_access", 3: "system.posix_acl_default",
            4: "trusted.", 6: "security.", 7: "system.", 8: "system.richacl", 200: ""}
SHARED_VALUE_BYTES = 32


def canonical(value):
    return json.loads(json.dumps(value))


def listed_name(value):
    # debugfs displays names containing non-ASCII bytes as a hex byte sequence.
    if re.fullmatch(r"(?:[0-9a-f]{2} )+", value):
        return bytes.fromhex(value).decode("utf-8")
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixtures", type=Path, required=True)
    parser.add_argument("--exports", type=Path, required=True)
    parser.add_argument("--tools-root", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    tools = resolve_tools(args.tools_root)
    fixtures = json.loads(args.fixtures.read_text())
    if not fixtures or not all(item.get("passed") for item in fixtures):
        raise RuntimeError("Expected independently verified xattr fixtures")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    records = []

    def save():
        (output / "report.json").write_text(json.dumps(records, indent=2) + "\n")

    for fixture in fixtures:
        source = Path(fixture["image"])
        expected = Path(fixture["expected"])
        images = {kind: args.exports.resolve() / f"xattr-{kind}-{source.name}"
                  for kind in ("shared-values", "unknown-namespace")}
        protected = {path: digest(path) for path in (source, expected, *images.values())}
        record = dict(source=str(source), source_sha256=protected[source], states={}, commands=[])
        records.append(record)
        save()

        def require(condition, message):
            if not condition:
                save()
                raise RuntimeError(f"{source.name}: {message}")

        def run(command, allowed=(0,)):
            done = subprocess.run([str(part) for part in command], capture_output=True,
                                  text=True, errors="backslashreplace", timeout=120)
            event = dict(command=[str(part) for part in command], status=done.returncode,
                         stdout=done.stdout, stderr=done.stderr)
            record["commands"].append(event)
            save()
            require(done.returncode in allowed, f"Independent command failed: {command}")
            return done

        require(protected[source] == fixture["input_sha256"] and
                protected[expected] == fixture["expected_sha256"], "Fixture changed before inspection")
        values = {}
        for line in expected.read_text().splitlines():
            path, namespace, name, payload = line.split()
            namespace = int(namespace)
            if namespace < 0:
                continue
            value_path = expected.parent / payload
            protected[value_path] = digest(value_path)
            name = "" if name == "-" else bytes.fromhex(name).decode("utf-8")
            values[path, namespace, name] = value_path.read_bytes()
        run([tools["e2fsck"], "-fn", source])
        counts = accounting(run([tools["dumpe2fs"], "-h", source]).stdout)
        for kind, image in images.items():
            target = dict(values)
            for path in ("/block", "/shared"):
                value = target.pop((path, 1, "binary"))
                if kind == "shared-values":
                    target[path, 1, "binary"] = value[:SHARED_VALUE_BYTES]
                    target[path, 1, "second"] = value[:SHARED_VALUE_BYTES]
                else:
                    target[path, 200, "binary"] = value
            check = run([tools["e2fsck"], "-fn", image], allowed=(4,) if kind == "shared-values" else (0,))
            if kind == "shared-values":
                # The reader intentionally accepts bounded shared values and zero
                # entry hashes. e2fsprogs reads them, but its checker rejects value
                # aliasing and the synthetic zero hash. This is compatibility
                # evidence, never a claim that the exported filesystem is clean.
                require("allocation collision" in check.stdout and "hash (0) which is invalid" in check.stdout,
                        "Synthetic shared-value diagnostic differs")
            require(accounting(run([tools["dumpe2fs"], "-h", image]).stdout) == counts,
                    "Reading representations changed allocation accounting")
            for path, inode in fixture["objects"].items():
                actual = inode_fields(run([tools["debugfs"], "-R", f"stat {path}", image]).stdout)
                require(canonical(actual) == inode, f"Unexpected inode change: {path}")
                listing = run([tools["debugfs"], "-R", f"ea_list {path}", image]).stdout
                entries = re.findall(r"^  (.+?) \((\d+)\)(?: =.*)?$", listing, re.M)
                wanted = {PREFIXES[namespace] + name: len(value)
                          for (owner, namespace, name), value in target.items() if owner == path}
                require(len(entries) == len(wanted) and {listed_name(name): int(size) for name, size in entries} == wanted,
                        f"Independent attribute list differs: {path}")
            for index, ((path, namespace, name), value) in enumerate(target.items()):
                extracted = output / f"{image.stem}-{index}.data"
                query = f'ea_get -r -f "{extracted}" {path} "{PREFIXES[namespace]}{name}"'
                run([tools["debugfs"], "-R", query, image])
                require(extracted.is_file() and extracted.read_bytes() == value,
                        f"Independent attribute bytes differ: {path} {namespace}:{name}")
            record["states"][kind] = dict(image=str(image), input_sha256=protected[image],
                                          passed=True, values=len(target), clean_e2fsck=check.returncode == 0,
                                          reader_compatibility_only=kind == "shared-values")
            save()
        require(all(digest(path) == before for path, before in protected.items()), "Inspection modified an input")
        record.update(passed=True, protected_inputs_unchanged=True,
                      protected_files=[dict(path=str(path), sha256=value) for path, value in protected.items()])
        save()
        print(f"PASS {source.name}: clean unknown namespace and separate shared-value reader compatibility", flush=True)


if __name__ == "__main__":
    main()
