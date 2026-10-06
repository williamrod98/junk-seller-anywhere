#!/usr/bin/env python3
"""Show, or set to neutral values, the user and machine fields of a Skyrim .pex header.

PapyrusCompiler.exe writes the compiling Windows user name and machine name into
every compiled script header. The released Junk Seller Anywhere scripts carry
the neutral values "build" and "AGHB" instead.

Usage:
    python3 tools/pex_header.py FILE.pex...                 # print source/user/machine
    python3 tools/pex_header.py --set-neutral FILE.pex...   # rewrite user/machine in place
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

PEX_MAGIC = bytes.fromhex("FA57C0DE")
NEUTRAL_USER = b"build"
NEUTRAL_MACHINE = b"AGHB"


def header_fields(data: bytes) -> tuple[bytes, bytes, bytes, int, int]:
    """Return (source, user, machine, user_offset, machine_offset).

    Layout: 4-byte magic, 2 version bytes, 2-byte game id, 8-byte compile time,
    then three strings, each with a big-endian uint16 length prefix.
    """
    if data[:4] != PEX_MAGIC:
        raise ValueError("not a Skyrim .pex file")
    if len(data) < 16:
        raise ValueError("truncated .pex header")
    offset = 16
    fields = []
    for name in ("source", "user", "machine"):
        if offset + 2 > len(data):
            raise ValueError(f"truncated .pex header at the {name} field")
        length = int.from_bytes(data[offset:offset + 2], "big")
        if offset + 2 + length > len(data):
            raise ValueError(f"truncated .pex header in the {name} field")
        fields.append((data[offset + 2:offset + 2 + length], offset))
        offset += 2 + length
    (source, _), (user, user_offset), (machine, machine_offset) = fields
    return source, user, machine, user_offset, machine_offset


def set_neutral(path: Path) -> bool:
    """Rewrite the user/machine strings; return True when the file changed."""
    data = path.read_bytes()
    _, user, machine, user_offset, machine_offset = header_fields(data)
    if user == NEUTRAL_USER and machine == NEUTRAL_MACHINE:
        return False
    tail = data[machine_offset + 2 + len(machine):]
    rebuilt = (data[:user_offset]
               + len(NEUTRAL_USER).to_bytes(2, "big") + NEUTRAL_USER
               + len(NEUTRAL_MACHINE).to_bytes(2, "big") + NEUTRAL_MACHINE + tail)
    path.write_bytes(rebuilt)
    return True


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--set-neutral", action="store_true", help="rewrite user/machine to build/AGHB")
    parser.add_argument("files", nargs="+", type=Path)
    args = parser.parse_args(argv)
    status = 0
    for path in args.files:
        try:
            if args.set_neutral:
                changed = set_neutral(path)
                print(f"{path}: {'changed' if changed else 'already neutral'}")
            else:
                source, user, machine, _, _ = header_fields(path.read_bytes())
                print(f"{path}: source={source.decode('latin-1')} user={user.decode('latin-1')} "
                      f"machine={machine.decode('latin-1')}")
        except (OSError, ValueError) as error:
            print(f"{path}: {error}", file=sys.stderr)
            status = 1
    return status


if __name__ == "__main__":
    raise SystemExit(main())
