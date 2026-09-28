#!/usr/bin/env python3
"""Select accepted bounded images with large logical files for the Linux VM."""

import argparse
import json
from pathlib import Path
import shutil

from check_orphans import digest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixtures", required=True, type=Path)
    parser.add_argument("--report", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    fixtures = json.loads(args.fixtures.read_text())
    checked = json.loads(args.report.read_text())
    if len(fixtures) != 9 or len(checked) != 36 or not all(row.get("passed") for row in fixtures + checked):
        raise RuntimeError("Missing accepted large-file matrix")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    selected = []
    for fixture in fixtures:
        # The pinned Linux reference uses 4 KiB pages. Larger filesystem blocks
        # remain a portable-core test, not a native mount acceptance claim.
        if fixture["block_size"] > 4096:
            continue
        matches = [row for row in checked if row["profile"] == fixture["profile"] and row["state"] == "written"]
        if len(matches) != 1:
            raise RuntimeError("Missing unique large-file native source")
        source = Path(matches[0]["image"])
        if digest(source) != matches[0]["image_sha256"]:
            raise RuntimeError("Checked large-file image changed")
        image = output / source.name
        shutil.copyfile(source, image)
        selected.append(dict(image=str(image), input_sha256=digest(image), source=str(source),
                             source_sha256=matches[0]["image_sha256"], block_size=fixture["block_size"], passed=True,
                             large_files={key: fixture[key] for key in
                                          ("profile", "block_size", "cluster_blocks", "limit", "segments")}))
    if len(selected) != 8:
        raise RuntimeError("Missing eight Linux-compatible large-file profiles")
    (output / "selection.json").write_text(json.dumps(selected, indent=2) + "\n")
    print("Prepared eight checked large-file images for the 4 KiB-page Linux reference", flush=True)


if __name__ == "__main__":
    main()
