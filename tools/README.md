# Spotify連携CSVツール

`spotify_link_from_csv.py` は、CSVのローカル曲情報から `foo_spotify_linker.db` の `track_map` へ直接登録する補助ツールです。

入力CSVは次の列を想定します。

```csv
path,file_size,title,artist,album,date,length_seconds,spotify_uri
```

`spotify_uri` が入っている行はそのまま登録します。`spotify_uri` が空で `--spotify-token` または環境変数 `SPOTIFY_ACCESS_TOKEN` がある場合は、Spotify検索で候補を探します。

```powershell
python tools\spotify_link_from_csv.py `
  --csv .\local_tracks.csv `
  --db "$env:APPDATA\foobar2000-v2\foo_spotify_linker.db" `
  --spotify-token $env:SPOTIFY_ACCESS_TOKEN `
  --review-csv .\auto_link_review.csv
```

書き込み前に確認する場合:

```powershell
python tools\spotify_link_from_csv.py --csv .\local_tracks.csv --db .\foo_spotify_linker.db --dry-run
```

`local_hash` はプラグイン側の `makeLocalHash()` と同じ入力順で再現しています。

