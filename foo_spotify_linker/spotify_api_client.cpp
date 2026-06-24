#include "stdafx.h"
#include "spotify_api_client.h"

namespace fsl
{
SpotifyResult SpotifyApiClient::play(const std::string &spotifyUri, double positionSeconds)
{
    FB2K_console_formatter() << "foo_spotify_linker: Spotify 再生要求 uri=" << spotifyUri.c_str()
                             << " pos=" << static_cast<int>(positionSeconds * 1000.0) << "ms";
    return {false, "Spotify Web API は未接続です。Client ID と OAuth 実装が必要です。"};
}

SpotifyResult SpotifyApiClient::pause()
{
    FB2K_console_formatter() << "foo_spotify_linker: Spotify pause 要求";
    return {false, "Spotify Web API は未接続です。"};
}

SpotifyResult SpotifyApiClient::seek(double positionSeconds)
{
    FB2K_console_formatter() << "foo_spotify_linker: Spotify seek 要求 pos="
                             << static_cast<int>(positionSeconds * 1000.0) << "ms";
    return {false, "Spotify Web API は未接続です。"};
}
} // namespace fsl
