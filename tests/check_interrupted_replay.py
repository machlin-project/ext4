#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Recover journals whose replay by e2fsck was interrupted.

e2fsck replays JBD2 logs with its copy of the kernel's jbd2 recovery code. A
preload library records the exact sequence of its writes and flushes to the image,
and the states a power cut could leave are rebuilt from that trace as described in
crash_states. While the log still holds
transactions, the core must recover each state to the same non-journal contents as
its own recovery of the untouched image, and strict fsck must accept the result.
Once e2fsck has emptied the log it continues with unjournaled processing such as
orphan release, which a power cut can tear. Mount-time recovery, the core's or
Linux's, does not audit every bitmap, so such a state may need fsck. Those states
are counted apart: the core may refuse one or leave what strict fsck reports, and
the state after all of e2fsck's writes must recover and pass strict fsck. e2fsck
and the core write different contents into released orphan records, so these
states are not compared byte for byte."""
import argparse
import hashlib
import itertools
import json
import os
from pathlib import Path
import platform
import shutil
import struct
import subprocess

from generate_fixtures import resolve_tools

TRACE_HEADER = struct.Struct("<QQ")
TRACE_WRITE = ord("W")
TRACE_FLUSH = ord("F")
# Epochs up to this size contribute every subset; larger ones contribute each
# single omission and each prefix.
EXHAUSTIVE_EPOCH = 10
SUPERBLOCK_OFFSET = 1024
LOG_BLOCK_SIZE_OFFSET = 24
MINIMUM_BLOCK_SIZE = 1024
# jbd2 superblock: s_start, big-endian, is zero once the log holds nothing to replay.
JOURNAL_START = struct.Struct(">I")
JOURNAL_START_OFFSET = 28


def digest(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def read_trace(path):
    data = path.read_bytes()
    events = []
    position = 0
    while position < len(data):
        kind = data[position]
        offset, length = TRACE_HEADER.unpack_from(data, position + 1)
        position += 1 + TRACE_HEADER.size
        if kind == TRACE_WRITE:
            events.append((offset, data[position:position + length]))
            position += length
        elif kind == TRACE_FLUSH:
            events.append(None)
        else:
            raise RuntimeError(f"{path}: malformed write trace")
    return events


def crash_states(original, events, block):
    """Yield (label, blocks) for every modeled interruption of the trace.

    Writes go through the page cache, so between two flushes they coalesce per
    filesystem block and reach the device in any order; a flush makes every earlier
    block durable. A power cut keeps any subset of the current epoch's dirty blocks
    on top of all earlier epochs, exhaustively for small epochs and otherwise every
    prefix in first-write order and every single omission."""
    epochs = [[]]
    for event in events:
        if event is None:
            epochs.append([])
        else:
            epochs[-1].append(event)
    durable = {}
    for index, epoch in enumerate(epochs):
        dirty = {}
        for offset, data in epoch:
            position = offset
            while position < offset + len(data):
                number = position // block
                start = position - number * block
                take = min(block - start, offset + len(data) - position)
                contents = bytearray(dirty.get(number) or durable.get(number) or
                                     original[number * block:(number + 1) * block])
                contents[start:start + take] = data[position - offset:position - offset + take]
                dirty[number] = bytes(contents)
                position += take
        numbers = list(dirty)
        if len(numbers) <= EXHAUSTIVE_EPOCH:
            subsets = [subset for size in range(len(numbers) + 1)
                       for subset in itertools.combinations(numbers, size)]
        else:
            subsets = [tuple(numbers[:count]) for count in range(len(numbers) + 1)]
            subsets += [tuple(number for number in numbers if number != omitted)
                        for omitted in numbers]
        for subset in subsets:
            state = dict(durable)
            state.update((number, dirty[number]) for number in subset)
            yield f"epoch-{index}-{len(subset)}-of-{len(numbers)}", state
        durable.update(dirty)
    yield "complete", durable


def build_library(output):
    source = Path(__file__).with_name("write_trace.c")
    compiler = os.environ.get("CC", "cc").split()
    if platform.system() == "Darwin":
        library = output / "write_trace.dylib"
        command = compiler + ["-dynamiclib", "-O2", "-Wall", "-Wextra", "-Werror", "-o",
                              str(library), str(source)]
        variable = "DYLD_INSERT_LIBRARIES"
    else:
        library = output / "write_trace.so"
        command = compiler + ["-shared", "-fPIC", "-O2", "-Wall", "-Wextra", "-Werror", "-o",
                              str(library), str(source), "-ldl"]
        variable = "LD_PRELOAD"
    subprocess.run(command, check=True)
    return library, variable, command


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixtures", type=Path,
                        help="generate_journal_fixtures.py output with manifest.json")
    parser.add_argument("--image", type=Path, action="append", default=[],
                        help="another pending image, such as one Linux left for recovery")
    parser.add_argument("--tools-root", type=Path, required=True)
    parser.add_argument("--recover", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    tools = resolve_tools(args.tools_root)
    recover = args.recover.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    library, variable, build = build_library(output)
    cases = []
    if args.fixtures:
        fixtures = args.fixtures.resolve()
        manifest = json.loads((fixtures / "manifest.json").read_text())
        for case in manifest["cases"]:
            source = fixtures / case["image"]
            if digest(source) != case["image_sha256"]:
                raise RuntimeError(f"fixture changed: {source}")
            cases.append((case["name"], source))
    cases += [(image.stem, image.resolve()) for image in args.image]
    if not cases:
        parser.error("select --fixtures or --image")
    rows = []

    def run(row, command, environment=None, allowed=(0,)):
        command = [str(part) for part in command]
        done = subprocess.run(command, capture_output=True, text=True, timeout=300,
                              env=environment)
        row["commands"].append(dict(command=command, status=done.returncode,
                                    stdout=done.stdout[-2000:], stderr=done.stderr[-2000:]))
        if done.returncode not in allowed:
            raise RuntimeError(f"Command failed ({done.returncode}): {command}\n"
                               f"{done.stdout[-1000:]}{done.stderr}")
        return done.stdout

    for name, source in cases:
        with source.open("rb") as stream:
            stream.seek(SUPERBLOCK_OFFSET + LOG_BLOCK_SIZE_OFFSET)
            block = MINIMUM_BLOCK_SIZE << struct.unpack("<I", stream.read(4))[0]
        row = dict(name=name, source=str(source), source_sha256=digest(source), build=build,
                   commands=[], passed=False, states=0, post_replay=0, refused=0,
                   left_for_fsck=0, post_replay_clean=0)
        rows.append(row)
        journal = {int(value) for value in
                   run(row, [tools["debugfs"], "-R", "blocks <8>", source]).split()}
        journal_super = int(run(row, [tools["debugfs"], "-R", "bmap <8> 0", source]).split()[-1])
        reference = output / f"{name}-reference.img"
        shutil.copyfile(source, reference)
        run(row, [recover, "--write", reference])
        run(row, [tools["e2fsck"], "-fn", reference])
        expected = reference.read_bytes()
        traced = output / f"{name}-e2fsck.img"
        trace = output / f"{name}.trace"
        shutil.copyfile(source, traced)
        environment = dict(os.environ, EXT4_WRITE_TRACE=str(trace),
                           EXT4_WRITE_TRACE_TARGET=str(traced), **{variable: str(library)})
        run(row, [tools["e2fsck"], "-E", "journal_only", "-y", traced], environment,
            allowed=(0, 1))
        events = read_trace(trace)
        row["writes"] = sum(event is not None for event in events)
        row["flushes"] = sum(event is None for event in events)
        if row["writes"] == 0:
            raise RuntimeError(f"{name}: e2fsck wrote nothing to the image")
        original = source.read_bytes()
        state_path = output / "state.img"
        seen = set()
        for label, blocks in crash_states(original, events, block):
            image = bytearray(original)
            for number, data in blocks.items():
                image[number * block:(number + 1) * block] = data
            key = hashlib.sha256(image).digest()
            if key in seen:
                continue
            seen.add(key)
            state_path.write_bytes(image)
            replaying = JOURNAL_START.unpack_from(
                image, journal_super * block + JOURNAL_START_OFFSET)[0] != 0
            row["state"] = label
            final = label == "complete"
            if not replaying:
                row["post_replay"] += 1
                run(row, [recover, "--write", state_path], allowed=(0,) if final else (0, 1))
                if row["commands"][-1]["status"] != 0:
                    row["refused"] += 1
                    continue
                run(row, [tools["e2fsck"], "-fn", state_path], allowed=(0,) if final else (0, 4))
                if row["commands"][-1]["status"] != 0:
                    row["left_for_fsck"] += 1
                else:
                    row["post_replay_clean"] += 1
                continue
            else:
                run(row, [recover, "--write", state_path])
                run(row, [tools["e2fsck"], "-fn", state_path])
            recovered = state_path.read_bytes()
            differing = [number for number in range(len(recovered) // block)
                         if number not in journal and
                         recovered[number * block:(number + 1) * block] !=
                         expected[number * block:(number + 1) * block]]
            if differing:
                raise RuntimeError(f"{name} {label}: blocks {differing[:8]} differ "
                                   "from the core's recovery of the untouched log")
            row["states"] += 1
            row["commands"] = row["commands"][-8:]
        for path in (reference, traced, state_path):
            path.unlink(missing_ok=True)
        row["passed"] = True
        (output / "report.json").write_text(json.dumps(rows, indent=2) + "\n")
        print(f"PASS interrupted e2fsck replay {name}: {row['writes']} writes, "
              f"{row['flushes']} flushes, {row['states']} recovered states, "
              f"{row['post_replay']} after replay: {row['post_replay_clean']} clean, "
              f"{row['refused']} refused, {row['left_for_fsck']} left for fsck",
              flush=True)


if __name__ == "__main__":
    main()
