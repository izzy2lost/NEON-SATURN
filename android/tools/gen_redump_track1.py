#!/usr/bin/env python3
"""Builds app/src/main/assets/covers/redump_track1.json from a Redump Saturn datfile.

The launcher hashes a game's first track and looks it up here to get the Redump name, which then picks the cover, so
covers no longer depend on how the files are named. Get the datfile from http://redump.org/datfile/ss/ and run:

    tools/gen_redump_track1.py "Sega - Saturn - Datfile (....).dat"
"""

import json
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

OUT = Path(__file__).resolve().parent.parent / "app/src/main/assets/covers/redump_track1.json"


def main() -> None:
    table: dict[str, str] = {}
    for game in ET.parse(sys.argv[1]).getroot().iter("game"):
        # Roms are listed in track order: the cue, then (Track 1).bin, (Track 2).bin, ...
        track1 = next((rom for rom in game.iter("rom") if rom.get("name", "").lower().endswith(".bin")), None)
        if track1 is None:
            continue
        # "crc32|size", the same key the launcher builds
        table.setdefault(f"{track1.get('crc').lower()}|{track1.get('size')}", game.get("name"))

    OUT.write_text(json.dumps(table, ensure_ascii=False, sort_keys=True, separators=(",", ":")) + "\n", encoding="utf-8")
    print(f"{len(table)} games -> {OUT}")


if __name__ == "__main__":
    main()
