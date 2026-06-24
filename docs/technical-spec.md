# foobar2000 ⇄ Spotify 連携プラグイン 技術仕様書

最終更新: 2026-06-25

---

## 0. 目的

ローカル音源を foobar2000 で再生した際に、同一曲を Spotify でも自動再生し、「Discord の Spotify ステータス」「Spotify のリスニング履歴」「友達アクティビティ」など外部サービスへローカル再生情報を反映させる。自動検索と**手動マッピング**の併用により、未収録曲や誤判定を最小化する。

---

## 1. 前提条件

| 項目 | 要件 |
| --- | --- |
| 対応 OS | Windows 10 / 11 64-bit |
| foobar2000 | v2.26 Preview 以降 |
| foobar2000 SDK | 2025-03-07 版 |
| Spotify アカウント | Premium (Web API 再生制御必須) |
| IDE | Visual Studio 2022 C++ Desktop |
| 依存ライブラリ | SQLite3, nlohmann::json, libcurl (WinHTTP 可), OpenSSL (PKCE)、fmtlib |

---

## 2. システム全体構成図

```text
┌───────────────┐      OAuth2 (PKCE)       ┌───────────────┐
│ foobar2000 UI │◀────────────────────────▶│ Spotify 授権  │
└────┬──────────┘       認証コード            └───────────────┘
     │play_callback                               ▲ access / refresh token
     ▼                                            │
┌──────────────────────────────────────────────────────────────────────┐
│          foo_spotify_linker (DLL / Component)                        │
│ ┌───────────────┐  ┌────────────────┐  ┌─────────────────────────┐ │
│ │ AuthManager   │  │ MappingManager │  │ SpotifyApiClient        │ │
│ └───────────────┘  └────────────────┘  └─────────────────────────┘ │
│           ▲                      ▲                  ▲              │
│           │                      │ SQLite           │ REST          │
│           └────────┬─────────────┴──────────────────┴──────────────┘
│                    │         track_map / album_map                 │
└────────────────────┴───────────────────────────────────────────────┘
```

---

## 3. データモデル

### 3.1 SQLite スキーマ

```sql
CREATE TABLE track_map (
  local_hash       TEXT PRIMARY KEY,
  spotify_uri      TEXT NOT NULL,
  updated_at       INTEGER NOT NULL  -- Unix epoch (ms)
);

CREATE TABLE album_map (
  album_id           TEXT PRIMARY KEY,
  spotify_album_uri  TEXT NOT NULL,
  updated_at         INTEGER NOT NULL
);

CREATE TABLE config (
  key   TEXT PRIMARY KEY,
  value TEXT NOT NULL
);
```

- **local_hash**: `SHA1(<absolute_path> + <filesize> + <tag_crc>)`
- **album_id**: `SHA1(<album_artist> + <album_title> + <year>)`

---

## 4. 機能要件

### 4.1 リアルタイム同期

| # | 条件 | 動作 |
| - | --- | --- |
| 1 | ローカルトラック開始 | Mapping/TrySearch → Spotify 再生開始 & `seek` |
| 2 | 曲位置ズレ > 500 ms | `PUT /me/player/seek` で補正 |
| 3 | 曲停止/スキップ | Spotify も `pause` 又は `next` |
| 4 | 未収録/検索失敗 | 同期スキップ & GUI 通知 |

### 4.2 手動マッピング UI

- **Preferences ▸ Spotify Linker**
  - データグリッド: Local Title / Spotify URI / State / Updated
  - ボタン: 追加・編集・削除・一括インポート / エクスポート
- **右クリックメニュー**
  - 現在トラックに「Spotify URIを設定…」
- **D&D**
  - Spotify から URI/URL をドロップで即登録

### 4.3 設定項目

| Key | 型 | 既定値 | 説明 |
| --- | --- | --- | --- |
| polling_interval_ms | int | 1000 | `/me/player` ポーリング周期 |
| default_device_id | string | - | 再生対象 Spotify デバイス |
| mute_on_sync | bool | true | 二重再生回避で Spotify 音量 0% |

---

## 5. モジュール詳細

### 5.1 AuthManager

| 関数 | 説明 |
| --- | --- |
| `BeginLogin()` | PKCE コードチャレンジ生成→ブラウザ起動 |
| `HandleRedirect(uri)` | `127.0.0.1` 受信→token 取得 |
| `GetAccessToken()` | 有効トークン返却、自動リフレッシュ |

### 5.2 SpotifyApiClient

ラッパー関数は C++17 以上で、`std::expected<>` でエラー処理する。

- `SearchTrack(q)`
- `Play(uri, pos_ms)`
- `Seek(pos_ms)`
- `TransferPlayback(device_id)`

429 レートリミット時は `Retry-After` で指数バックオフする。

### 5.3 MappingManager

| 操作 | 挙動 |
| --- | --- |
| `Resolve(local_hash, metadata)` | 1. `track_map` → 2. `album_map` → 3. 検索 |
| `AddMapping(local_hash, spotify_uri)` | upsert |
| `ExportCSV(path)` / `ImportCSV(path)` | BOM-UTF8 CSV |

---

## 6. ビルド & デプロイ

### 6.1 依存ライブラリの取得

```powershell
vcpkg install sqlite3 nlohmann-json curl openssl fmt
```

### 6.2 ビルドスクリプト `build.ps1`

[foo_monthly_stats の build.ps1](https://github.com/shirafukayayoi/foo_monthly_stats/blob/main/build.ps1) を雛形に、プロジェクト名・パスを置き換えて再利用する。

主な処理フロー:

1. **MSBuild 検出**: `vswhere.exe` で Visual Studio のインストールパスを取得し、`MSBuild.exe` を特定。
2. **DLL ビルド**: `foo_spotify_linker\foo_spotify_linker.vcxproj` を `/p:Configuration=$Config /p:Platform=$Platform` でビルド。
3. **テスト**: `tests\tests.vcxproj` をビルドし、`foo_spotify_linker_tests.exe` を実行。
4. **パッケージング**: `scripts\pack_component.py` を呼び出して `foo_spotify_linker.fb2k-component` を生成。
5. **成果物表示**: 成功時に DLL と component のパスを表示。

実行例:

```powershell
.\build.ps1 -Config Release -Platform x64
```

### 6.3 CI/CD (GitHub Actions 例)

```yaml
name: Windows Build
on: [push, pull_request]
jobs:
  build:
    runs-on: windows-latest
    steps:
      - uses: actions/checkout@v4
      - uses: lukka/run-vcpkg@v11
      - name: Build & Package
        shell: pwsh
        run: ./build.ps1 -Config Release -Platform x64
      - uses: actions/upload-artifact@v4
        with:
          name: foo_spotify_linker_fb2k
          path: _result/**/foo_spotify_linker.fb2k-component
```

---

## 7. テスト計画

| 種別 | 代表テストケース |
| --- | --- |
| 単体 | Auth 成功/失敗、DB CRUD、URI 正規化 |
| 結合 | 再生同期、ズレ補正、二重再生無音確認 |
| 負荷 | 1 時間連続再生 & 曲送り 1000 回、429 応答シミュレート |
| UI | 手動マッピング追加/削除/編集、D&D 受け取り |

---

## 8. コーディング規約

- C++20 構造化束縛、`std::span`, `std::expected` 利用
- 命名: PascalCase (class)、camelCase (func/var)
- 文字列は UTF-8; Windows API 呼び出しは `std::wstring` に変換
- clang-format (`Google`) 準拠

---

## 9. 今後の拡張候補

- プレイリスト単位の双方向同期
- Spotify のアートワークを foobar2000 カラム UI に反映
- MIDI/HID コントローラショートカット

---

## 10. 納品物

| ファイル | 説明 |
| --- | --- |
| `/src/*` | C++ ソース一式 |
| `README.md` | ビルド手順・FAQ |
| `LICENSE` | MIT |
| `foo_spotify_linker.fb2k-component` | リリースビルド成果物 |
