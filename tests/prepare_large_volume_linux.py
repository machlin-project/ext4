#!/usr/bin/env python3
"""Select checked sparse high-address images without reading their holes."""

import argparse
import json
from pathlib import Path

from sparse_image import sparse_copy, sparse_digest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixtures", required=True, type=Path)
    parser.add_argument("--report", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    fixtures = json.loads(args.fixtures.read_text())
    checked = json.loads(args.report.read_text())
    if len(fixtures) != 3 or len(checked) != 3 or not all(row.get("passed") for row in fixtures + checked):
        raise RuntimeError("Missing accepted high-address profiles")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    selected = []
    for fixture in fixtures:
        matches = [row for row in checked if row["profile"] == fixture["profile"]]
        if len(matches) != 1:
            raise RuntimeError("Missing unique high-address profile")
        states = {row["label"]: row for row in matches[0]["states"]}
        if len(states) != 7:
            raise RuntimeError("High-address fixture has incomplete independent states")
        record = dict(block_size=fixture["block_size"], passed=True,
                      image_digest_format="ext4-test-sparse-pages-v1",
                      large_volume={key: fixture[key] for key in
                                    ("profile", "block_size", "cluster_blocks", "blocks",
                                     "first_high_block", "directory_inode", "seed_inode")})
        for label, key in (("mutated", "image"), ("after-core", "core_recovered")):
            source = Path(states[label]["image"])
            if sparse_digest(source) != states[label]["sparse_sha256"]:
                raise RuntimeError("Independently checked high-address state changed")
            image = output / f"{fixture['profile']}-{label}.img"
            sparse_copy(source, image)
            record[key] = str(image)
            record["input_sha256" if key == "image" else f"{key}_sha256"] = sparse_digest(image)
        source = Path(states["after-core"]["image"]).with_name("after-pending.img")
        image = output / f"{fixture['profile']}-pending.img"
        before = sparse_digest(source)
        sparse_copy(source, image)
        if sparse_digest(image) != before or sparse_digest(source) != before:
            raise RuntimeError("Sparse pending-journal copy changed contents")
        record.update(core_pending=str(image), core_pending_sha256=before)
        selected.append(record)
    (output / "selection.json").write_text(json.dumps(selected, indent=2) + "\n")
    print("Prepared three high-address Linux roundtrips and portable pending journals", flush=True)


if __name__ == "__main__":
    main()
