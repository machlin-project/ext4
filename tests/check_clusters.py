#!/usr/bin/env python3
"""Check clustered allocation mutations against exact independent inode state."""

import argparse
import copy
import json
from pathlib import Path
import subprocess

from check_orphans import digest
from generate_cluster_fixtures import payload
from generate_fixtures import resolve_tools
from linux_xattrs import SECTOR_BYTES, snapshot

SECONDS = 1700000300
STATES = ("sparse", "file", "attributes", "lifetime")


def verify(before, after, state, block, ratio):
    wanted = copy.deepcopy(before)
    cluster = block * ratio
    if state == "lifetime":
        changed = wanted["objects"]["/empty"]
        changed["inode"].update(mtime=(SECONDS, 0), ctime=(SECONDS, 0))
    elif state == "sparse":
        changed = wanted["objects"]["/sparse"]
        value = bytearray.fromhex(changed["data"])
        value[:cluster] = bytes(cluster)
        value[block:2 * block] = payload(block)
        changed["data"] = value.hex()
        changed["inode"].update(mode=0o640, mtime=(SECONDS, 0), ctime=(SECONDS, 0))
    else:
        changed = wanted["objects"]["/file"]
        value = bytearray(2 * cluster + 9)
        value[:block + 7] = payload(block + 7)
        value[cluster - 3:cluster + block + 5] = b"\xd7" * (block + 8)
        changed["data"] = value.hex()
        changed["inode"].update(mode=0o640, size=len(value), blocks=3 * cluster // SECTOR_BYTES,
                                 mtime=(SECONDS, 0), ctime=(SECONDS, 0))
        wanted["accounting"]["Free blocks"] += 2 * ratio
        if state == "attributes":
            changed["attrs"]["user.value"] = payload(7).hex()
            changed["inode"]["blocks"] = 4 * cluster // SECTOR_BYTES
            wanted["accounting"]["Free blocks"] -= ratio
    if wanted != after:
        differences = [path for path in set(wanted["objects"]) | set(after["objects"])
                       if wanted["objects"].get(path) != after["objects"].get(path)]
        raise RuntimeError(f"Clustered {state} state differs: objects={differences}, "
                           f"expected allocation={wanted['accounting']}, actual={after['accounting']}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixtures", required=True, type=Path)
    parser.add_argument("--exports", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--tools-root", type=Path)
    args = parser.parse_args()
    fixtures = json.loads(args.fixtures.read_text())
    if not fixtures or not all(row.get("passed") for row in fixtures):
        raise RuntimeError("Unverified clustered fixture input")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    tools = resolve_tools(args.tools_root)
    reports = []
    for fixture in fixtures:
        source = Path(fixture["image"])
        if digest(source) != fixture["image_sha256"]:
            raise RuntimeError("Clustered fixture changed")
        baseline = None
        for state in STATES:
            image = args.exports.resolve() / f"cluster-{state}-{source.name}"
            record = dict(image=str(image), source=str(source), state=state, commands=[])
            reports.append(record)

            def run(command, allowed=(0,)):
                result = subprocess.run([str(x) for x in command], capture_output=True,
                                        text=True, errors="backslashreplace", timeout=120)
                record["commands"].append(dict(command=[str(x) for x in command], status=result.returncode,
                                                stdout=result.stdout, stderr=result.stderr))
                if result.returncode not in allowed:
                    raise RuntimeError(f"Independent clustered command failed: {command}")
                return result.stdout

            try:
                original_hash = digest(image)
                if baseline is None:
                    baseline = snapshot(source, output / f"{source.stem}-before", tools, run)
                observed = snapshot(image, output / f"{image.stem}-state", tools, run)
                verify(baseline, observed, state, fixture["block_size"], fixture["cluster_blocks"])
                if digest(image) != original_hash or digest(source) != fixture["image_sha256"]:
                    raise RuntimeError("Read-only cluster verification changed media")
                record.update(passed=True, image_sha256=original_hash,
                              source_sha256=fixture["image_sha256"], allocation=observed["accounting"])
                print(f"PASS {image.name}: exact objects, cluster accounting and nonrepairing fsck", flush=True)
            except Exception as error:
                record.update(passed=False, error=str(error))
                raise
            finally:
                (output / "report.json").write_text(json.dumps(reports, indent=2) + "\n")


if __name__ == "__main__":
    main()
