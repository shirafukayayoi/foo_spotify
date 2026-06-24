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
- Spotify URL の正規化 (`https://open.spotify.com/intl-ja/track/...` などのロケール付き URL を含む)
- アルバム単位マッピング時のトラック番号オフセット再生
- `spotify:track:...` の仮想トラック再生
- Spotify 現在再生を foobar2000 仮想トラックへ追従する実験機能
- 再生開始、停止、pause、seek イベントの検知
- `.fb2k-component` パッケージ作成

未実装です。

- Spotify プレイリスト取り込み UI
- D&D 登録 UI

## Spotify マッピング

foobar2000 上でトラックを右クリックし、`Spotify Linker` メニューから登録します。

- `Set Track URI...`: 選択トラック単位で Spotify track URI / URL を登録
- `Auto Set Track URI`: 選択トラックのタグから Spotify track を検索して登録
- `Remove Track URI`: 選択トラック単位の登録を削除
- `Set Album URI...`: 選択トラックのアルバム単位で Spotify album URI / URL を登録
- `Auto Set Album URI`: 選択トラックのアルバムタグから Spotify album を検索して登録
- `Remove Album URI`: 選択トラックのアルバム単位の登録を削除

アルバム単位で登録した場合は、foobar2000 側の `TRACKNUMBER` を使って Spotify アルバム内の同じ位置から再生します。たとえばローカルの 3 曲目を再生した場合、Spotify 側もアルバムの 3 曲目から再生します。

## Spotify 仮想トラック

foobar2000 の `File > Add Location...` などから `spotify:track:...` を追加すると、foobar2000 側では無音の仮想トラックとして再生時間を進め、実際の音は Spotify のアクティブデバイスで再生します。

```text
spotify:track:2VU59VkXEBNX4ZZf7SmGAy
```

この方式では foobar2000 側も再生中になるため、Playback Statistics 系の再生時間加算対象にできます。ただし Spotify 音源を foobar2000 でデコードしているわけではなく、foobar2000 は無音 PCM を流し、Spotify Web API で再生、pause、seek、音量を同期します。仮想トラック再生時の Spotify 音量は foobar2000 の現在音量に合わせます。

`Preferences > Tools > Spotify Linker` の `Follow Spotify playback in foobar2000` を有効にすると、Spotify 側で現在再生中の track を polling し、foobar2000 側でも `spotify:track:...` 仮想トラックとして再生開始します。Jam 専用 API は Spotify Web API にないため、Jam か通常再生かは判定せず「自分の Spotify アカウントの現在再生」を追従します。

対応 URL 例:

```text
https://open.spotify.com/intl-ja/track/2VU59VkXEBNX4ZZf7SmGAy?si=0d0b3e910e1e486b
https://open.spotify.com/track/2VU59VkXEBNX4ZZf7SmGAy
spotify:track:2VU59VkXEBNX4ZZf7SmGAy
spotify:album:...
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

## ビルド

```powershell
.\build.ps1 -Config Release -Platform x64
```

成果物:

- `foo_spotify_linker\_result\x64_Release\bin\foo_spotify_linker.dll`
- `foo_spotify_linker\_result\foo_spotify_linker.fb2k-component`

## License

MIT License
