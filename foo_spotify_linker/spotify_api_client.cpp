#include "stdafx.h"
#include "auth_manager.h"
#include "http_client.h"
#include "settings.h"
#include "spotify_api_client.h"

namespace fsl
{
namespace
{
std::vector<std::wstring> bearerHeaders(const std::string &token, bool json = false)
{
    std::wstring auth = L"Authorization: Bearer ";
    auth += std::wstring(token.begin(), token.end());
    std::vector<std::wstring> headers{auth};
    if (json)
        headers.push_back(L"Content-Type: application/json");
    return headers;
}

SpotifyResult callPlayerApi(const wchar_t *method, const std::wstring &url, const std::string &body, bool json)
{
    std::string token;
    std::string message;
    if (!AuthManager::instance().ensureAccessToken(token, message))
        return {false, message};

    const auto response = httpRequest(method, url, bearerHeaders(token, json), body);
    if (!response)
        return {false, "Spotify Web API へ接続できませんでした。"};
    if (response->status == 204 || (response->status >= 200 && response->status < 300))
        return {true, "OK"};
    return {false, "Spotify Web API error HTTP " + std::to_string(response->status) + ": " + response->body};
}

std::wstring playbackUrl(const std::wstring &pathAndQuery)
{
    return L"https://api.spotify.com/v1/me/player" + pathAndQuery;
}
} // namespace

SpotifyResult SpotifyApiClient::play(const std::string &spotifyUri, double positionSeconds)
{
    const int posMs = static_cast<int>(positionSeconds * 1000.0);
    std::wstring url = playbackUrl(L"/play");
    pfc::string8 device = g_cfg_default_device_id.get();
    if (!device.is_empty())
        url += L"?device_id=" + std::wstring(device.c_str(), device.c_str() + std::strlen(device.c_str()));

    if (g_cfg_mute_on_sync.get())
        setVolume(0);

    const std::string body = "{\"uris\":[\"" + spotifyUri + "\"],\"position_ms\":" + std::to_string(posMs) + "}";
    const SpotifyResult result = callPlayerApi(L"PUT", url, body, true);
    if (!result.ok)
        FB2K_console_formatter() << "foo_spotify_linker: Spotify play failed: " << result.message.c_str();
    return result;
}

SpotifyResult SpotifyApiClient::pause()
{
    std::wstring url = playbackUrl(L"/pause");
    pfc::string8 device = g_cfg_default_device_id.get();
    if (!device.is_empty())
        url += L"?device_id=" + std::wstring(device.c_str(), device.c_str() + std::strlen(device.c_str()));
    return callPlayerApi(L"PUT", url, "", false);
}

SpotifyResult SpotifyApiClient::seek(double positionSeconds)
{
    const int posMs = static_cast<int>(positionSeconds * 1000.0);
    std::wstring url = playbackUrl(L"/seek?position_ms=" + std::to_wstring(posMs));
    pfc::string8 device = g_cfg_default_device_id.get();
    if (!device.is_empty())
        url += L"&device_id=" + std::wstring(device.c_str(), device.c_str() + std::strlen(device.c_str()));
    return callPlayerApi(L"PUT", url, "", false);
}

SpotifyResult SpotifyApiClient::setVolume(int volumePercent)
{
    if (volumePercent < 0)
        volumePercent = 0;
    if (volumePercent > 100)
        volumePercent = 100;
    std::wstring url = playbackUrl(L"/volume?volume_percent=" + std::to_wstring(volumePercent));
    pfc::string8 device = g_cfg_default_device_id.get();
    if (!device.is_empty())
        url += L"&device_id=" + std::wstring(device.c_str(), device.c_str() + std::strlen(device.c_str()));
    return callPlayerApi(L"PUT", url, "", false);
}
} // namespace fsl
