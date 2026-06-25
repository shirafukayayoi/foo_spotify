#!/usr/bin/env python3
"""CSVからfoo_spotify_linkerのtrack_mapへSpotify連携を登録する補助ツール。"""

from __future__ import annotations

import argparse
import csv
import json
import os
import re
import sqlite3
import sys
import time
import urllib.parse
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


FNV32_PRIME = 16777619
FNV32_MASK = 0xFFFFFFFF


@dataclass
class LocalTrack:
    path: str
    file_size: int
    title: str
    artist: str
    album: str
    date: str
    length_seconds: float = 0.0


@dataclass
class SpotifyCandidate:
    uri: str
    title: str
    artist: str
    album: str
    duration_ms: int
    score: int
    reason: str


def fnv1a(text: str, seed: int) -> int:
    value = seed & FNV32_MASK
    for byte in text.encode("utf-8"):
        value ^= byte
        value = (value * FNV32_PRIME) & FNV32_MASK
    return value


def pseudo_sha1(text: str) -> str:
    h1 = fnv1a(text, 2166136261)
    h2 = fnv1a(text, h1 ^ 0x9E3779B9)
    h3 = fnv1a(text, h2 ^ 0x85EBCA6B)
    h4 = fnv1a(text, h3 ^ 0xC2B2AE35)
    h5 = fnv1a(text, h4 ^ 0x27D4EB2F)
    return f"{h1:08x}{h2:08x}{h3:08x}{h4:08x}{h5:08x}"


def make_local_hash(track: LocalTrack) -> str:
    text = "\n".join(
        [
            track.path,
            str(track.file_size),
            track.title,
            track.artist,
            track.album,
            track.date,
        ]
    )
    return pseudo_sha1(text)


def normalize_text(value: str) -> str:
    return " ".join(value.casefold().split())


def spotify_uri_from_value(value: str) -> str:
    value = (value or "").strip()
    if re.fullmatch(r"spotify:track:[A-Za-z0-9]{22}", value):
        return value
    match = re.search(r"/track/([A-Za-z0-9]{22})", value)
    if match:
        return f"spotify:track:{match.group(1)}"
    return ""


def duration_close(local_seconds: float, spotify_ms: int, tolerance: float) -> bool:
    if local_seconds <= 0 or spotify_ms <= 0:
        return False
    return abs(local_seconds - (spotify_ms / 1000.0)) <= tolerance


def score_candidate(track: LocalTrack, item: dict, duration_tolerance: float) -> SpotifyCandidate:
    title = str(item.get("name") or "")
    album = str((item.get("album") or {}).get("name") or "")
    artists = item.get("artists") or []
    artist = ", ".join(str(a.get("name") or "") for a in artists if isinstance(a, dict))
    uri = str(item.get("uri") or "")
    duration_ms = int(item.get("duration_ms") or 0)

    score = 0
    reasons: list[str] = []
    if normalize_text(track.title) and normalize_text(track.title) == normalize_text(title):
        score += 60
        reasons.append("title")
    if normalize_text(track.artist) and normalize_text(track.artist) == normalize_text(artist):
        score += 30
        reasons.append("artist")
    if normalize_text(track.album) and normalize_text(track.album) == normalize_text(album):
        score += 15
        reasons.append("album")
    if duration_close(track.length_seconds, duration_ms, duration_tolerance):
        score += 15
        reasons.append("duration")
    return SpotifyCandidate(uri, title, artist, album, duration_ms, score, "+".join(reasons))


def build_queries(track: LocalTrack) -> list[str]:
    queries: list[str] = []

    def add(query: str) -> None:
        if query and query not in queries:
            queries.append(query)

    if track.title and track.artist:
        add(f'track:"{track.title}" artist:"{track.artist}"')
    if track.title and track.album:
        add(f'track:"{track.title}" album:"{track.album}"')
    if track.title:
        add(f'track:"{track.title}"')
        add(track.title)
    return queries


def spotify_search(access_token: str, query: str, limit: int) -> list[dict]:
    url = "https://api.spotify.com/v1/search?" + urllib.parse.urlencode(
        {"type": "track", "limit": str(limit), "q": query}
    )
    request = urllib.request.Request(url, headers={"Authorization": f"Bearer {access_token}"})
    with urllib.request.urlopen(request, timeout=20) as response:
        payload = json.loads(response.read().decode("utf-8"))
    return list((payload.get("tracks") or {}).get("items") or [])


def find_best_candidate(
    track: LocalTrack,
    access_token: str,
    limit: int,
    min_score: int,
    duration_tolerance: float,
) -> SpotifyCandidate | None:
    best: SpotifyCandidate | None = None
    seen: set[str] = set()
    for query in build_queries(track):
        try:
            items = spotify_search(access_token, query, limit)
        except Exception as exc:
            print(f"search failed: {track.artist} - {track.title}: {exc}", file=sys.stderr)
            continue
        for item in items:
            uri = str(item.get("uri") or "")
            if not uri or uri in seen:
                continue
            seen.add(uri)
            candidate = score_candidate(track, item, duration_tolerance)
            if best is None or candidate.score > best.score:
                best = candidate
        if best and best.score >= min_score:
            return best
    return best if best and best.score >= min_score else None


def row_value(row: dict[str, str], *names: str) -> str:
    lowered = {key.strip().casefold(): value for key, value in row.items()}
    for name in names:
        value = lowered.get(name.casefold())
        if value is not None:
            return value.strip()
    return ""


def parse_float(value: str) -> float:
    try:
        return float(value)
    except ValueError:
        return 0.0


def parse_int(value: str) -> int:
    try:
        return int(float(value))
    except ValueError:
        return 0


def read_tracks(csv_path: Path) -> Iterable[tuple[LocalTrack, str]]:
    with csv_path.open("r", encoding="utf-8-sig", newline="") as handle:
        reader = csv.DictReader(handle)
        for line_number, row in enumerate(reader, start=2):
            path = row_value(row, "path", "file", "filename", "location")
            title = row_value(row, "title", "TITLE")
            artist = row_value(row, "artist", "ARTIST")
            album = row_value(row, "album", "ALBUM")
            date = row_value(row, "date", "DATE", "year")
            file_size = parse_int(row_value(row, "file_size", "filesize", "size"))
            if not file_size and path and Path(path).exists():
                file_size = Path(path).stat().st_size
            length = parse_float(row_value(row, "length_seconds", "duration", "length"))
            spotify_uri = spotify_uri_from_value(row_value(row, "spotify_uri", "spotify", "uri", "spotify_url"))

            if not path or not title or not artist:
                print(f"skip line {line_number}: path/title/artist が不足しています", file=sys.stderr)
                continue
            yield LocalTrack(path, file_size, title, artist, album, date, length), spotify_uri


def ensure_schema(connection: sqlite3.Connection) -> None:
    connection.execute(
        "CREATE TABLE IF NOT EXISTS track_map ("
        "local_hash TEXT PRIMARY KEY,"
        "spotify_uri TEXT NOT NULL,"
        "updated_at INTEGER NOT NULL)"
    )


def upsert_mapping(connection: sqlite3.Connection, local_hash: str, spotify_uri: str) -> None:
    connection.execute(
        "INSERT INTO track_map(local_hash, spotify_uri, updated_at) VALUES(?, ?, ?) "
        "ON CONFLICT(local_hash) DO UPDATE SET "
        "spotify_uri = excluded.spotify_uri, updated_at = excluded.updated_at",
        (local_hash, spotify_uri, int(time.time() * 1000)),
    )


def write_review_header(path: Path) -> None:
    with path.open("w", encoding="utf-8-sig", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(
            [
                "path",
                "title",
                "artist",
                "album",
                "selected_spotify_uri",
                "spotify_title",
                "spotify_artist",
                "spotify_album",
                "score",
                "reason",
            ]
        )


def append_review(path: Path, track: LocalTrack, candidate: SpotifyCandidate | None) -> None:
    with path.open("a", encoding="utf-8-sig", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(
            [
                track.path,
                track.title,
                track.artist,
                track.album,
                candidate.uri if candidate else "",
                candidate.title if candidate else "",
                candidate.artist if candidate else "",
                candidate.album if candidate else "",
                candidate.score if candidate else "",
                candidate.reason if candidate else "no-match",
            ]
        )


def main() -> int:
    parser = argparse.ArgumentParser(description="CSVからSpotify連携をfoo_spotify_linker DBへ登録します。")
    parser.add_argument("--csv", required=True, type=Path, help="入力CSV。path,title,artist,album,date,file_size列を推奨。")
    parser.add_argument("--db", required=True, type=Path, help="foo_spotify_linker.db のパス。")
    parser.add_argument("--spotify-token", default=os.environ.get("SPOTIFY_ACCESS_TOKEN", ""), help="検索に使うSpotify access token。")
    parser.add_argument("--review-csv", type=Path, help="検索結果や未登録行を書き出すCSV。")
    parser.add_argument("--dry-run", action="store_true", help="DBへ書き込まず結果だけ表示します。")
    parser.add_argument("--limit", type=int, default=5, help="Spotify検索候補数。")
    parser.add_argument("--min-score", type=int, default=75, help="自動登録する最低スコア。")
    parser.add_argument("--duration-tolerance", type=float, default=3.0, help="曲長一致の許容秒数。")
    args = parser.parse_args()

    if args.review_csv:
        write_review_header(args.review_csv)

    registered = 0
    skipped = 0
    searched = 0
    connection = sqlite3.connect(args.db)
    try:
        ensure_schema(connection)
        for track, csv_uri in read_tracks(args.csv):
            spotify_uri = csv_uri
            candidate: SpotifyCandidate | None = None
            if not spotify_uri and args.spotify_token:
                candidate = find_best_candidate(track, args.spotify_token, args.limit, args.min_score, args.duration_tolerance)
                if candidate:
                    spotify_uri = candidate.uri
                searched += 1

            if args.review_csv:
                append_review(args.review_csv, track, candidate)

            if not spotify_uri:
                skipped += 1
                continue
            local_hash = make_local_hash(track)
            if args.dry_run:
                print(f"DRY {local_hash} -> {spotify_uri} / {track.artist} - {track.title}")
            else:
                upsert_mapping(connection, local_hash, spotify_uri)
            registered += 1
        if not args.dry_run:
            connection.commit()
    finally:
        connection.close()

    print(f"registered={registered} skipped={skipped} searched={searched} dry_run={args.dry_run}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
