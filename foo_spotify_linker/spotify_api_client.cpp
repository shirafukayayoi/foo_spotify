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

std::optional<HttpResponse> callSpotifyGet(const std::wstring &url)
{
    std::string token;
    std::string message;
    if (!AuthManager::instance().ensureAccessToken(token, message))
    {
        FB2K_console_formatter() << "foo_spotify_linker: Spotify token error: " << message.c_str();
        return std::nullopt;
    }
    return httpRequest(L"GET", url, bearerHeaders(token, false), "");
}

std::wstring playbackUrl(const std::wstring &pathAndQuery)
{
    return L"https://api.spotify.com/v1/me/player" + pathAndQuery;
}

std::optional<std::string> firstSpotifyUri(const std::string &json, const std::string &prefix)
{
    const std::string marker = "\"uri\"";
    size_t pos = 0;
    while ((pos = json.find(marker, pos)) != std::string::npos)
    {
        pos = json.find(':', pos + marker.size());
        if (pos == std::string::npos)
            return std::nullopt;
        pos = json.find('"', pos + 1);
        if (pos == std::string::npos)
            return std::nullopt;
        const size_t end = json.find('"', pos + 1);
        if (end == std::string::npos)
            return std::nullopt;

        const std::string value = json.substr(pos + 1, end - pos - 1);
        if (value.rfind(prefix, 0) == 0)
            return value;
        pos = end + 1;
    }
    return std::nullopt;
}

std::optional<std::string> jsonStringValueAfter(const std::string &json, const std::string &anchor, const std::string &key)
{
    const size_t anchorPos = anchor.empty() ? 0 : json.find(anchor);
    if (anchorPos == std::string::npos)
        return std::nullopt;

    const std::string marker = "\"" + key + "\"";
    size_t pos = json.find(marker, anchorPos);
    if (pos == std::string::npos)
        return std::nullopt;
    pos = json.find(':', pos + marker.size());
    if (pos == std::string::npos)
        return std::nullopt;
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos)
        return std::nullopt;
    const size_t end = json.find('"', pos + 1);
    if (end == std::string::npos)
        return std::nullopt;
    return json.substr(pos + 1, end - pos - 1);
}

std::optional<int> jsonIntValueLocal(const std::string &json, const std::string &key)
{
    const std::string marker = "\"" + key + "\"";
    size_t pos = json.find(marker);
    if (pos == std::string::npos)
        return std::nullopt;
    pos = json.find(':', pos + marker.size());
    if (pos == std::string::npos)
        return std::nullopt;
    ++pos;
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos])))
        ++pos;
    size_t end = pos;
    while (end < json.size() && std::isdigit(static_cast<unsigned char>(json[end])))
        ++end;
    if (end == pos)
        return std::nullopt;
    return std::atoi(json.substr(pos, end - pos).c_str());
}

std::optional<bool> jsonBoolValueLocal(const std::string &json, const std::string &key)
{
    const std::string marker = "\"" + key + "\"";
    size_t pos = json.find(marker);
    if (pos == std::string::npos)
        return std::nullopt;
    pos = json.find(':', pos + marker.size());
    if (pos == std::string::npos)
        return std::nullopt;
    ++pos;
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos])))
        ++pos;
    if (json.compare(pos, 4, "true") == 0)
        return true;
    if (json.compare(pos, 5, "false") == 0)
        return false;
    return std::nullopt;
}

std::optional<std::string> trackIdFromUri(const std::string &spotifyTrackUri)
{
    const std::string prefix = "spotify:track:";
    if (spotifyTrackUri.rfind(prefix, 0) != 0 || spotifyTrackUri.size() <= prefix.size())
        return std::nullopt;
    return spotifyTrackUri.substr(prefix.size());
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

    std::string body;
    const std::string albumPrefix = "spotify:album:";
    if (spotifyUri.rfind(albumPrefix, 0) == 0)
    {
        body = "{\"context_uri\":\"" + spotifyUri + "\",\"position_ms\":" + std::to_string(posMs) + "}";
    }
    else
    {
        body = "{\"uris\":[\"" + spotifyUri + "\"],\"position_ms\":" + std::to_string(posMs) + "}";
    }
    const SpotifyResult result = callPlayerApi(L"PUT", url, body, true);
    if (!result.ok)
        FB2K_console_formatter() << "foo_spotify_linker: Spotify play failed: " << result.message.c_str();
    return result;
}

SpotifyResult SpotifyApiClient::playVirtualTrack(const std::string &spotifyTrackUri, double positionSeconds, int volumePercent)
{
    const int posMs = static_cast<int>(positionSeconds * 1000.0);
    std::wstring url = playbackUrl(L"/play");
    pfc::string8 device = g_cfg_default_device_id.get();
    if (!device.is_empty())
        url += L"?device_id=" + std::wstring(device.c_str(), device.c_str() + std::strlen(device.c_str()));

    setVolume(volumePercent);
    const std::string body = "{\"uris\":[\"" + spotifyTrackUri + "\"],\"position_ms\":" + std::to_string(posMs) + "}";
    const SpotifyResult result = callPlayerApi(L"PUT", url, body, true);
    if (result.ok)
        setVolume(volumePercent);
    else
        FB2K_console_formatter() << "foo_spotify_linker: Spotify virtual play failed: " << result.message.c_str();
    return result;
}

SpotifyResult SpotifyApiClient::playAlbum(const std::string &spotifyAlbumUri, int zeroBasedOffset, double positionSeconds)
{
    if (zeroBasedOffset < 0)
        zeroBasedOffset = 0;
    const int posMs = static_cast<int>(positionSeconds * 1000.0);
    std::wstring url = playbackUrl(L"/play");
    pfc::string8 device = g_cfg_default_device_id.get();
    if (!device.is_empty())
        url += L"?device_id=" + std::wstring(device.c_str(), device.c_str() + std::strlen(device.c_str()));

    if (g_cfg_mute_on_sync.get())
        setVolume(0);

    const std::string body = "{\"context_uri\":\"" + spotifyAlbumUri + "\",\"offset\":{\"position\":" +
                             std::to_string(zeroBasedOffset) + "},\"position_ms\":" + std::to_string(posMs) + "}";
    const SpotifyResult result = callPlayerApi(L"PUT", url, body, true);
    if (!result.ok)
        FB2K_console_formatter() << "foo_spotify_linker: Spotify album play failed: " << result.message.c_str();
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

std::optional<std::string> SpotifyApiClient::searchTrack(const std::string &query)
{
    const std::string encoded = urlEncode(query);
    const std::wstring url = L"https://api.spotify.com/v1/search?type=track&limit=1&q=" +
                             std::wstring(encoded.begin(), encoded.end());
    const auto response = callSpotifyGet(url);
    if (!response || response->status < 200 || response->status >= 300)
        return std::nullopt;
    return firstSpotifyUri(response->body, "spotify:track:");
}

std::optional<std::string> SpotifyApiClient::searchAlbum(const std::string &query)
{
    const std::string encoded = urlEncode(query);
    const std::wstring url = L"https://api.spotify.com/v1/search?type=album&limit=1&q=" +
                             std::wstring(encoded.begin(), encoded.end());
    const auto response = callSpotifyGet(url);
    if (!response || response->status < 200 || response->status >= 300)
        return std::nullopt;
    return firstSpotifyUri(response->body, "spotify:album:");
}

std::optional<SpotifyTrackInfo> SpotifyApiClient::getTrackInfo(const std::string &spotifyTrackUri)
{
    const auto id = trackIdFromUri(spotifyTrackUri);
    if (!id)
        return std::nullopt;

    const std::wstring url = L"https://api.spotify.com/v1/tracks/" + std::wstring(id->begin(), id->end());
    const auto response = callSpotifyGet(url);
    if (!response || response->status < 200 || response->status >= 300)
        return std::nullopt;

    SpotifyTrackInfo info;
    info.uri = spotifyTrackUri;
    if (const auto title = jsonStringValueAfter(response->body, "", "name"))
        info.title = *title;
    if (const auto artist = jsonStringValueAfter(response->body, "\"artists\"", "name"))
        info.artist = *artist;
    if (const auto album = jsonStringValueAfter(response->body, "\"album\"", "name"))
        info.album = *album;
    if (const auto duration = jsonIntValueLocal(response->body, "duration_ms"))
        info.durationMs = *duration;
    return info;
}

std::optional<SpotifyPlaybackInfo> SpotifyApiClient::getCurrentPlayback()
{
    const auto response = callSpotifyGet(L"https://api.spotify.com/v1/me/player/currently-playing");
    if (!response || response->status == 204 || response->status < 200 || response->status >= 300)
        return std::nullopt;

    const auto uri = firstSpotifyUri(response->body, "spotify:track:");
    if (!uri)
        return std::nullopt;

    SpotifyPlaybackInfo info;
    info.trackUri = *uri;
    if (const auto progress = jsonIntValueLocal(response->body, "progress_ms"))
        info.progressMs = *progress;
    if (const auto playing = jsonBoolValueLocal(response->body, "is_playing"))
        info.isPlaying = *playing;
    return info;
}
} // namespace fsl
