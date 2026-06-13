#!/usr/bin/env python3
"""Download GBA box-art covers for a folder of ROMs.

Covers come from the libretro thumbnails project, matched by the ROM's
file name (which is expected to follow the No-Intro naming standard, e.g.
"Pokemon - FireRed Version (USA).gba"). Images are saved as
<games_dir>/covers/<rom-stem>.png, which is exactly where the emulator's
game-library grid looks for them.

Usage:
    python3 tools/fetch_covers.py [games_dir]   # defaults to current dir
"""
import os
import sys
import urllib.parse
import urllib.request

HOST = "https://thumbnails.libretro.com"
DIR = "/Nintendo - Game Boy Advance/Named_Boxarts/"
ROM_EXTS = {".gba", ".agb"}


def fetch(stem, out_path):
    # Encode the whole path (the directory has spaces too); keep the slashes.
    url = HOST + urllib.parse.quote(DIR + stem + ".png")
    req = urllib.request.Request(url, headers={"User-Agent": "gba_emu-covers"})
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            data = resp.read()
    except urllib.error.HTTPError as e:
        if e.code == 404:
            return False
        raise
    with open(out_path, "wb") as f:
        f.write(data)
    return True


def main():
    games_dir = sys.argv[1] if len(sys.argv) > 1 else "."
    covers_dir = os.path.join(games_dir, "covers")
    os.makedirs(covers_dir, exist_ok=True)

    roms = sorted(f for f in os.listdir(games_dir)
                  if os.path.splitext(f)[1].lower() in ROM_EXTS)
    if not roms:
        print(f"No ROMs found in {games_dir!r}")
        return

    got = missing = skipped = 0
    for rom in roms:
        stem = os.path.splitext(rom)[0]
        out_path = os.path.join(covers_dir, stem + ".png")
        if os.path.exists(out_path):
            print(f"  skip   {stem} (already have cover)")
            skipped += 1
            continue
        if fetch(stem, out_path):
            print(f"  got    {stem}")
            got += 1
        else:
            print(f"  miss   {stem} (no cover in libretro database)")
            missing += 1

    print(f"\nDone: {got} downloaded, {skipped} skipped, {missing} missing "
          f"-> {covers_dir}")


if __name__ == "__main__":
    main()
