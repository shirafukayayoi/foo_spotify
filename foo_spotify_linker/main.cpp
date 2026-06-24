#include "stdafx.h"

DECLARE_COMPONENT_VERSION(
    "Spotify Linker",
    "0.1.0",
    "foo_spotify_linker\n"
    "foobar2000 のローカル再生を Spotify 再生状態へ同期する component。\n"
    "現在はマッピング管理と再生イベント連携の MVP 実装です。\n"
    "https://github.com/shirafukayayoi/foo_spotify");

VALIDATE_COMPONENT_FILENAME("foo_spotify_linker.dll");

FOOBAR2000_IMPLEMENT_CFG_VAR_DOWNGRADE;
