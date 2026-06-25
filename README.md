# foo_spotify

foobar2000 で再生しているローカル音源を Spotify の再生状態へ同期するための foobar2000 component 計画リポジトリです。

## 目的

ローカル音源を foobar2000 で再生した際に、同一曲を Spotify でも自動再生し、Discord の Spotify ステータス、Spotify のリスニング履歴、友達アクティビティなどへローカル再生情報を反映させます。

自動検索と手動マッピングを併用し、Spotify 未収録曲や誤判定を最小化します。

## 想定環境

| 項目 | 要件 |
| --- | --- |
| OS | Windows 10 / 11 64-bit |
| foobar2000 | v2.26 Preview 以降 |
| foobar2000 SDK | 2025-03-07 版 |
| Spotify | Premium アカウント |
| IDE | Visual Studio 2022 C++ Desktop |
| 依存ライブラリ | SQLite3, nlohmann::json, libcurl, OpenSSL, fmtlib |

## 予定機能

- foobar2000 の再生開始に合わせて Spotify でも同一曲を再生
- 再生位置のズレ補正
- 停止、スキップ操作の同期
- Spotify URI による手動マッピング
- SQLite による曲、アルバム単位のマッピング保存
- CSV インポート、エクスポート

## 技術仕様

詳細は [docs/technical-spec.md](docs/technical-spec.md) を参照してください。

## 依存ライブラリの取得

```powershell
vcpkg install sqlite3 nlohmann-json curl openssl fmt
```

## 状態

現在は MVP 実装段階です。以下は実装済みです。

- foobar2000 component としてロード可能な DLL のビルド
- `Preferences > Tools > Spotify Linker` の基本設定画面
- Spotify OAuth PKCE login
- OAuth access token / refresh token の foobar2000 config 保存
- Redirect URI `http://127.0.0.1:8088/callback`
- Spotify Web API による再生、pause、seek、音量制御
- SQLite による `track_map` / `album_map` / `config` テーブル作成
- 右クリックメニュー `Spotify Linker` からの Spotify track / album URI 手動登録、自動登録、削除
- Track URI手動登録で album URL / URI を入力した場合、選択曲のトラック番号に対応する Spotify track URI へ解決して登録
- Spotify URL の正規化 (`https://open.spotify.com/intl-ja/track/...` などのロケール付き URL を含む)
- `File > Add > Spotify Linker > Add Spotify Link...` からの Spotify track / album / playlist / Jam リンク追加
- `File > Add > Spotify Linker > Auto Link Library Tracks` からの Media Library 一括自動連携
- Spotify playlist URL / URI の track 仮想トラック展開
- `https://spotify.link/...` / `https://spotify.app.link/...` 短縮リンクの展開
- アルバム単位マッピング時のトラック番号オフセット再生
- `spotify:track:...` の仮想トラック再生
- Spotify 現在再生を foobar2000 仮想トラックへ追従する実験機能
- 再生開始、停止、pause、seek イベントの検知
- `.fb2k-component` パッケージ作成

未実装です。

- D&D 登録 UI

## Spotify マッピング

foobar2000 上でトラックを右クリックし、`Spotify Linker` メニューから登録します。

- `Set Track URI...`: 選択トラック単位で Spotify track URI / URL を登録
- `Auto Set Track URI`: 選択トラックのタグから Spotify track を検索して登録
- `Show Track URI`: 選択トラック単位で登録済みの Spotify track URI を表示
- `Remove Track URI`: 選択トラック単位の登録を削除
- `Set Album URI...`: 選択トラックのアルバム単位で Spotify album URI / URL を登録
- `Auto Set Album URI`: 選択トラックのアルバムタグから Spotify album を検索して登録
- `Show Album URI`: 選択トラックのアルバム単位で登録済みの Spotify album URI を表示
- `Remove Album URI`: 選択トラックのアルバム単位の登録を削除

アルバム単位で登録した場合は、foobar2000 側の `TRACKNUMBER` を使って Spotify アルバム内の同じ位置から再生します。たとえばローカルの 3 曲目を再生した場合、Spotify 側もアルバムの 3 曲目から再生します。

## Spotify 仮想トラック

foobar2000 の `File > Add > Spotify Linker > Add Spotify Link...` から Spotify URL / URI を追加できます。`File > Add Location...` に直接入れる必要はありません。この入口から追加した内容は、今開いている playlist ではなく、新しく作成した playlist に入ります。

track / album は URI として追加します。playlist は Spotify Web API で曲一覧を取得し、各曲を `spotify:track:...` の仮想トラックとして新規 playlist へ追加します。
`Spotify Playlist` / `Spotify Jam` / `Spotify Album` / `Spotify Track` という専用 playlist 内では、曲が最後まで再生された時点でその item を playlist から自動削除します。ローカル曲でも専用 playlist 内であれば同じ扱いです。

```text
https://open.spotify.com/playlist/...
https://spotify.link/...
spotify:track:2VU59VkXEBNX4ZZf7SmGAy
spotify:playlist:...
```

この方式では foobar2000 側も再生中になるため、Playback Statistics 系の再生時間加算対象にできます。ただし Spotify 音源を foobar2000 でデコードしているわけではなく、foobar2000 は無音 PCM を流し、Spotify Web API で再生、pause、seek、音量を同期します。仮想トラック再生時の Spotify 音量は foobar2000 の現在音量に合わせます。
仮想トラックのアルバムアートは Spotify track 情報の `album.images` から取得します。初回表示時に画像を取得し、以後は URI / 画像 URL 単位でメモリキャッシュします。
Spotify 側で同一曲リピートが有効な場合、仮想トラック終端後にSpotify側の再生位置が曲頭へ戻ったことを検出すると、foobar2000側の仮想トラックも先頭へ戻して再生を継続します。

Jam 招待リンク (`open.spotify.com/socialsession/...` や、その `spotify.link` 短縮 URL) は Spotify Web API から固定の曲一覧を直接取得できません。このプラグインでは Jam リンクを track と誤判定せず、Spotify 側で現在再生中の曲と queue 先頭の次曲だけを `Spotify Jam` playlist へ追加します。既に `Spotify Jam` playlist があればそれを再利用します。`Preferences > Tools > Spotify Linker` の `Follow Spotify playback in foobar2000` を有効にすると、Jam 専用 playlist で1曲再生し終わるたびに Spotify queue 先頭から次の1曲を補充します。

`Follow Spotify playback in foobar2000` を有効にすると、Spotify 側で現在再生中の track を polling し、foobar2000 側でも `spotify:track:...` 仮想トラックとして再生開始します。あわせて Spotify の queue 先頭に見える次曲を active playlist へ1曲ずつ追加します。Jam 専用 API は Spotify Web API にないため、Jam か通常再生かは判定せず「自分の Spotify アカウントの現在再生」と「自分の Spotify queue」を追従します。
foobar2000 起動直後の最初の polling では、Spotify の現在再生と queue を既読として記録するだけで、playlist への追加や再生開始は行いません。これにより、Spotify 側で Jam を開いたまま foobar2000 を起動しても、起動直後に大量追加されることを避けます。
常時追従で拾えない場合は、`File > Add > Spotify Linker > Add Current Spotify Playback` を実行すると、Spotify 側の現在再生中トラックと queue 先頭の次曲を手動で `Spotify Playlist` に追加できます。

`Auto Link Library Tracks` は foobar2000 Media Library 内のローカルファイルだけをバックグラウンドで走査し、未登録の曲だけ Spotify track と自動連携します。YouTube Source などの remote / non-local source は対象外です。Spotify 検索の上位候補を再取得し、タイトルが一致したうえで、アーティスト、アルバム、または曲の長さが合う候補を登録します。Spotify 側のアーティスト名が英字表記で、ローカルタグが日本語表記の場合に備えて、検索はアーティスト込みの条件から始め、見つからない場合は track + album や track 主体の条件へフォールバックします。

プラグイン内の自動連携で取りきれない曲は、`tools/spotify_link_from_csv.py` でCSVから `track_map` へ直接登録できます。使い方は [tools/README.md](tools/README.md) を参照してください。

対応 URL 例:

```text
https://open.spotify.com/intl-ja/track/2VU59VkXEBNX4ZZf7SmGAy?si=0d0b3e910e1e486b
https://open.spotify.com/track/2VU59VkXEBNX4ZZf7SmGAy
spotify:track:2VU59VkXEBNX4ZZf7SmGAy
spotify:album:...
spotify:playlist:...
```

## Spotify Developer App 設定

Spotify Developer Dashboard 側で Redirect URI に以下を登録してください。

```text
http://127.0.0.1:8088/callback
```

プラグイン側では `Preferences > Tools > Spotify Linker` に Spotify App の Client ID を入力し、`Login Spotify` を押します。

要求 scope:

- `user-modify-playback-state`
- `user-read-playback-state`
- `playlist-read-private`
- `playlist-read-collaborative`

既存ログイン済み環境で playlist 取り込みが失敗する場合は、追加 scope を反映するために `Login Spotify` を再実行してください。

## ビルド

```powershell
.\build.ps1 -Config Release -Platform x64
```

成果物:

- `foo_spotify_linker\_result\x64_Release\bin\foo_spotify_linker.dll`
- `foo_spotify_linker\_result\foo_spotify_linker.fb2k-component`

## License

MIT License
