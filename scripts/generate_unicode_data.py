#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Generate the ext4 utf8-12.1 casefold tables from the Unicode Character Database.

ext4 casefolding applies the full (C and F) case folding, canonical decomposition
(NFD, without compatibility mappings) and canonical ordering, and removes code
points with the Default_Ignorable_Code_Point property. Code points without 12.1 data,
including unassigned ones, map to themselves; only malformed UTF-8 or a surrogate
makes a name opaque. Hangul syllables decompose algorithmically and are not stored.
"""
import argparse
import hashlib
from pathlib import Path

VERSION = (12, 1)
SOURCES = {
    "UnicodeData.txt": "93ab1acd8fd9d450463b50ae77eab151a7cda48f98b25b56baed8070f80fc936",
    "CaseFolding.txt": "9c772627c6ee77eea6a17b42927b8ee28ca05dc65d6a511062104baaf3d12294",
    "DerivedCoreProperties.txt":
        "a6eb7a8671fb532fbd88c37fd7b20b5b2e7dbfc8b121f74c14abe2947db0da68",
    "DerivedAge.txt": "2fc081011d8fabaf7cf4937732dd5a6d6a57e492c43f3adfeded513387ee0ec3",
}
URL = "https://www.unicode.org/Public/12.1.0/ucd/"
HANGUL_FIRST, HANGUL_LAST = 0xAC00, 0xD7A3
SURROGATES = range(0xD800, 0xE000)


def records(path):
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.split("#", 1)[0].strip()
        if line:
            yield [field.strip() for field in line.split(";")]


def code_range(text):
    first, _, last = text.partition("..")
    return range(int(first, 16), int(last or first, 16) + 1)


def load(directory):
    for name, expected in SOURCES.items():
        if hashlib.sha256((directory / name).read_bytes()).hexdigest() != expected:
            raise RuntimeError(f"{name} is not the pinned Unicode {VERSION} file from {URL}")
    classes = {}
    decompositions = {}
    pending = None
    for fields in records(directory / "UnicodeData.txt"):
        code = int(fields[0], 16)
        if fields[1].endswith(", First>"):
            pending = code
            continue
        span = range(pending, code + 1) if fields[1].endswith(", Last>") else (code,)
        pending = None
        for point in span:
            if int(fields[3]):
                classes[point] = int(fields[3])
            if fields[5] and not fields[5].startswith("<"):
                decompositions[point] = [int(part, 16) for part in fields[5].split()]
    folds = {}
    for fields in records(directory / "CaseFolding.txt"):
        if fields[1] in ("C", "F"):
            folds[int(fields[0], 16)] = [int(part, 16) for part in fields[2].split()]
    ignorable = set()
    for fields in records(directory / "DerivedCoreProperties.txt"):
        if fields[1] == "Default_Ignorable_Code_Point":
            ignorable.update(code_range(fields[0]))
    assigned = set()
    for fields in records(directory / "DerivedAge.txt"):
        if tuple(int(part) for part in fields[1].split(".")) <= VERSION:
            assigned.update(code_range(fields[0]))
    assigned.difference_update(SURROGATES)
    return classes, decompositions, folds, ignorable, assigned


def hangul(code):
    index = code - HANGUL_FIRST
    parts = [0x1100 + index // 588, 0x1161 + index % 588 // 28]
    if index % 28:
        parts.append(0x11A7 + index % 28)
    return parts


def mappings(decompositions, folds, ignorable, assigned):
    def canonical(code):
        if code in ignorable:
            return []
        if HANGUL_FIRST <= code <= HANGUL_LAST:
            return hangul(code)
        if code in decompositions:
            return [part for item in decompositions[code] for part in canonical(item)]
        return [code]

    def folded(code, depth=0):
        if depth > 8:
            raise RuntimeError(f"Casefold expansion of {code:04X} does not converge")
        if code in ignorable:
            return []
        if code in folds:
            return [part for item in folds[code] for part in folded(item, depth + 1)]
        return canonical(code)

    result = {}
    for code in sorted(set(decompositions) | set(folds)):
        if HANGUL_FIRST <= code <= HANGUL_LAST or code in ignorable:
            continue
        mapping = folded(code)
        if mapping != [code]:
            result[code] = mapping
    return result


def ranges(points):
    output = []
    for point in sorted(points):
        if output and output[-1][1] + 1 == point:
            output[-1][1] = point
        else:
            output.append([point, point])
    return output


def emit(classes, table, assigned, ignorable):
    pool = []
    rows = []
    for code, mapping in sorted(table.items()):
        rows.append((code, len(pool), len(mapping)))
        pool.extend(mapping)
    class_ranges = []
    for code in sorted(classes):
        if code in assigned and class_ranges and class_ranges[-1][1] + 1 == code and \
                class_ranges[-1][2] == classes[code]:
            class_ranges[-1][1] = code
        elif code in assigned:
            class_ranges.append([code, code, classes[code]])
    lines = [
        "/* SPDX-License-Identifier: BSD-3-Clause */",
        "/* Generated by scripts/generate_unicode_data.py from the Unicode Character",
        " * Database 12.1.0 (UnicodeData, CaseFolding, DerivedCoreProperties and",
        " * DerivedAge). Do not edit. Unicode data is used under the Unicode license. */",
        "#ifndef MACHLIN_EXT4_UNICODE_DATA_H",
        "#define MACHLIN_EXT4_UNICODE_DATA_H",
        "",
        "#define EXT4_UNICODE_IGNORABLE_COUNT %dU" % len(ranges(ignorable)),
        "#define EXT4_UNICODE_CLASS_COUNT %dU" % len(class_ranges),
        "#define EXT4_UNICODE_MAPPING_COUNT %dU" % len(rows),
        "#define EXT4_UNICODE_POOL_COUNT %dU" % len(pool),
        "#define EXT4_UNICODE_MAPPING_MAX %dU" % max(length for _, _, length in rows),
        "",
        "/* Default ignorable code points, including unassigned ones, are removed. */",
        "static const struct ext4_unicode_range ext4_unicode_ignorable[] = {",
    ]
    lines += [f"\t{{ 0x{first:04x}U, 0x{last:04x}U }}," for first, last in ranges(ignorable)]
    lines += ["};", "", "static const struct ext4_unicode_class ext4_unicode_classes[] = {"]
    lines += [f"\t{{ 0x{first:04x}U, 0x{last:04x}U, {value}U }},"
              for first, last, value in class_ranges]
    lines += ["};", "", "static const struct ext4_unicode_mapping ext4_unicode_mappings[] = {"]
    lines += [f"\t{{ 0x{code:04x}U, {offset}U, {length}U }}," for code, offset, length in rows]
    lines += ["};", "", "static const uint32_t ext4_unicode_pool[] = {"]
    for start in range(0, len(pool), 8):
        lines.append("\t" + " ".join(f"0x{code:04x}U," for code in pool[start:start + 8]))
    lines += ["};", "", "#endif", ""]
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ucd", type=Path, required=True,
                        help=f"directory holding the Unicode 12.1.0 files from {URL}")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    classes, decompositions, folds, ignorable, assigned = load(args.ucd)
    table = mappings(decompositions, folds, ignorable, assigned)
    args.output.write_text(emit(classes, table, assigned, ignorable))


if __name__ == "__main__":
    main()
