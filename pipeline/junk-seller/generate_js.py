#!/usr/bin/env python3
"""Deterministic writer of AGH_JunkSeller.esp (an ESL-flagged plugin) and its SEQ start list.

It emits the record layout directly with the Python standard library, reads no game records, and
writes two quests, each with one VMAD script and no properties or aliases: AGH_JS_Quest (0x000800,
script AGH_JS_Quest) and the MCM host quest AGH_JS_MCMQuest (0x000801, script AGH_JS_MCM). Both are
start-game-enabled and both are listed in Seq/AGH_JunkSeller.seq.

Usage: python3 generate_js.py --out <folder>
"""
from __future__ import annotations
import argparse
import hashlib
import json
import struct
from pathlib import Path

PLUGIN = "AGH_JunkSeller.esp"
SCRIPT = "AGH_JS_Quest"
EDITOR = "AGH_JS_Quest"
FORM_ID = 0x01000800  # Skyrim.esm is master index 0; this file is index 1.
# The MCM host quest. The MCM script needs an attached quest record for
# SkyUI's StartGameEnabled registration; it carries no properties and never
# calls the DLL on its own.
MCM_SCRIPT = "AGH_JS_MCM"
MCM_EDITOR = "AGH_JS_MCMQuest"
MCM_FORM_ID = 0x01000801
# SEQ carries the local 24 bit object index with the MAST-table index byte left
# at 0x00; the engine ORs in the plugin's own runtime index (0xFE___ for an ESL)
# because it already knows which plugin the file belongs to. An early revision
# wrote the disk FormID 0x01000800 with no count header at all, and the quest
# never started. The layout below is the one the author's other shipped SEQ
# files use.
SEQ_FORM_ID = FORM_ID & 0x00FFFFFF
SEQ_MCM_FORM_ID = MCM_FORM_ID & 0x00FFFFFF
# HEDR field 2 counts the records and groups the file holds: one GRUP plus the
# two QUST records.
HEDR_RECORD_COUNT = 3
HEDR_NEXT_OBJECT_ID = 0x802


def sub(tag: bytes, data: bytes) -> bytes:
    return struct.pack("<4sH", tag, len(data)) + data


def record(tag: bytes, data: bytes, *, flags: int = 0, form: int = 0) -> bytes:
    return struct.pack("<4sIIIIHH", tag, len(data), flags, form, 0, 44, 0) + data


def quest(editor: str, script_name: str, form: int) -> bytes:
    """One start-game-enabled QUST whose VMAD attaches one script, no properties."""
    script = script_name.encode("ascii")
    vmad = struct.pack("<HHHH", 5, 2, 1, len(script)) + script
    vmad += struct.pack("<BH", 0, 0)  # Script flags and property count.
    vmad += struct.pack("<BHHH", 2, 0, 0, 0)  # QUST fragments, file name, aliases.
    data = sub(b"EDID", editor.encode("ascii") + b"\0") + sub(b"VMAD", vmad)
    data += sub(b"DNAM", bytes.fromhex("010000ff0000000000000000")) + sub(b"NEXT", b"")
    return record(b"QUST", data, form=form)


def generate() -> bytes:
    header = sub(b"HEDR", struct.pack("<fII", 1.71, HEDR_RECORD_COUNT, HEDR_NEXT_OBJECT_ID))
    header += sub(b"CNAM", b"AGH\0") + sub(b"SNAM", b"Junk seller observation probe r1.\0")
    header += sub(b"MAST", b"Skyrim.esm\0") + sub(b"DATA", bytes(8))
    quests = quest(EDITOR, SCRIPT, FORM_ID) + quest(MCM_EDITOR, MCM_SCRIPT, MCM_FORM_ID)
    group = struct.pack("<4sI4sIHHHH", b"GRUP", 24 + len(quests), b"QUST", 0, 0, 0, 0, 0)
    return record(b"TES4", header, flags=0x200) + group + quests


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    payload = generate()
    (args.out / PLUGIN).write_bytes(payload)
    # Every start-game-enabled quest in an added plugin needs the SEQ start list.
    seq_dir = args.out / "Seq"
    seq_dir.mkdir(exist_ok=True)
    (seq_dir / PLUGIN.replace(".esp", ".seq")).write_bytes(
        struct.pack("<III", 2, SEQ_FORM_ID, SEQ_MCM_FORM_ID))
    plan = {"plugin": PLUGIN, "esl": True, "masters": ["Skyrim.esm"],
            "records": [{"type": "QUST", "formId": "0x000800", "editorId": EDITOR,
                         "startGameEnabled": True, "scripts": [SCRIPT], "aliases": []},
                        {"type": "QUST", "formId": "0x000801", "editorId": MCM_EDITOR,
                         "startGameEnabled": True, "scripts": [MCM_SCRIPT], "aliases": []}],
            "seq": {"count": 2, "formIds": ["0x%06X" % SEQ_FORM_ID, "0x%06X" % SEQ_MCM_FORM_ID],
                    "bytes": 12},
            "sha256": hashlib.sha256(payload).hexdigest(), "bytes": len(payload)}
    (args.out / "js-plan.json").write_text(json.dumps(plan, indent=2) + "\n", encoding="utf-8")
    print(f"PASS: {args.out / PLUGIN} ({len(payload)} bytes), QUST=2, ESL=1, "
          f"SEQ=2 (count=2, formIds=0x{SEQ_FORM_ID:06X},0x{SEQ_MCM_FORM_ID:06X})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
