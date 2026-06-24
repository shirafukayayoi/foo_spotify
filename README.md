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

現在は技術仕様の公開段階です。実装、ビルドスクリプト、CI は今後追加予定です。

## License

MIT License
