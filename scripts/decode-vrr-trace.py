#!/usr/bin/env python3
"""Expand a chunk-compressed Moonlight VRR trace to CSV."""

from __future__ import annotations

import argparse
import struct
import sys
import zlib
from pathlib import Path
from typing import BinaryIO


MAGIC = b"MLVRR1\n"


def decode(source: BinaryIO, destination: BinaryIO) -> None:
    magic = source.read(len(MAGIC))
    if magic != MAGIC:
        raise ValueError("not a Moonlight chunk-compressed VRR trace")

    chunk_number = 0
    while True:
        encoded_length = source.read(4)
        if not encoded_length:
            return
        if len(encoded_length) != 4:
            raise ValueError("trace ends in a partial chunk length")

        chunk_number += 1
        compressed_length = struct.unpack("<I", encoded_length)[0]
        payload = source.read(compressed_length)
        if len(payload) != compressed_length:
            raise ValueError(f"trace ends in partial chunk {chunk_number}")
        if len(payload) < 4:
            raise ValueError(f"chunk {chunk_number} has no size prefix")

        expected_length = struct.unpack(">I", payload[:4])[0]
        expanded = zlib.decompress(payload[4:])
        if len(expanded) != expected_length:
            raise ValueError(
                f"chunk {chunk_number} expanded to {len(expanded)} bytes, "
                f"expected {expected_length}"
            )
        destination.write(expanded)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path, help="input .vrrtrace file")
    parser.add_argument(
        "csv",
        nargs="?",
        type=Path,
        help="output CSV (stdout when omitted)",
    )
    args = parser.parse_args()

    try:
        with args.trace.open("rb") as source:
            if args.csv is None:
                decode(source, sys.stdout.buffer)
            else:
                with args.csv.open("wb") as destination:
                    decode(source, destination)
    except (OSError, ValueError, zlib.error) as error:
        parser.exit(1, f"decode-vrr-trace: {error}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
