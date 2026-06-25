#!/usr/bin/env python3
"""ローカル音源タグをfoo_spotify_linker用CSVへ書き出す補助ツール。"""

from __future__ import annotations

import argparse
import csv
from pathlib import Path

from mutagen import File


AUDIO_EXTS = {".mp3", ".flac", ".m4a", ".mp4", ".ogg", ".opus", ".wav", ".wma", ".aac"}


def first(tags, *names: str) -> str:
    if not tags:
        return ""
    for name in names:
        value = tags.get(name)
        if value is None:
            continue
        if isinstance(value, list):
            return str(value[0]) if value else ""
        if hasattr(value, "text"):
            text = getattr(value, "text")
            if isinstance(text, list):
                return str(text[0]) if text else ""
            return str(text)
        return str(value)
    return ""


def export_music(roots: list[Path], output: Path) -> tuple[int, int]:
    total = 0
    written = 0
    with output.open("w", encoding="utf-8-sig", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(["path", "file_size", "title", "artist", "album", "date", "length_seconds", "spotify_uri"])
        for root in roots:
            for path in root.rglob("*"):
                if not path.is_file() or path.suffix.lower() not in AUDIO_EXTS:
                    continue
                total += 1
                try:
                    audio = File(path, easy=True)
                    tags = audio.tags if audio else {}
                    title = first(tags, "title")
                    artist = first(tags, "artist", "albumartist")
                    album = first(tags, "album")
                    date = first(tags, "date", "year")
                    length = getattr(getattr(audio, "info", None), "length", 0.0) if audio else 0.0
                    if not title or not artist:
                        continue
                    writer.writerow([str(path), path.stat().st_size, title, artist, album, date, f"{length:.3f}", ""])
                    written += 1
                except Exception:
                    continue
    return total, written


def main() -> int:
    parser = argparse.ArgumentParser(description="ローカル音源タグをCSVへ書き出します。")
    parser.add_argument("--root", action="append", required=True, type=Path, help="音源ルート。複数指定可。")
    parser.add_argument("--out", required=True, type=Path, help="出力CSV。")
    args = parser.parse_args()
    total, written = export_music(args.root, args.out)
    print(f"total={total} written={written} out={args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
