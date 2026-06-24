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

std::vector<std::string> spotifyUris(const std::string &json, const std::string &prefix)
{
    std::vector<std::string> values;
    const std::string marker = "\"uri\"";
    size_t pos = 0;
    while ((pos = json.find(marker, pos)) != std::string::npos)
    {
        pos = json.find(':', pos + marker.size());
        if (pos == std::string::npos)
            break;
        pos = json.find('"', pos + 1);
        if (pos == std::string::npos)
            break;
        const size_t end = json.find('"', pos + 1);
        if (end == std::string::npos)
            break;

        const std::string value = json.substr(pos + 1, end - pos - 1);
        if (value.rfind(prefix, 0) == 0 && std::find(values.begin(), values.end(), value) == values.end())
            values.push_back(value);
        pos = end + 1;
    }
    return values;
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

std::optional<std::string> jsonStringValueInRange(const std::string &json, const std::string &key, size_t begin, size_t end)
{
    const std::string marker = "\"" + key + "\"";
    size_t pos = json.find(marker, begin);
    if (pos == std::string::npos || pos >= end)
        return std::nullopt;
    pos = json.find(':', pos + marker.size());
    if (pos == std::string::npos || pos >= end)
        return std::nullopt;
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos || pos >= end)
        return std::nullopt;

    std::string out;
    bool escape = false;
    for (++pos; pos < end; ++pos)
    {
        const char ch = json[pos];
        if (escape)
        {
            out.push_back(ch);
            escape = false;
            continue;
        }
        if (ch == '\\')
        {
            escape = true;
            continue;
        }
        if (ch == '"')
            return out;
        out.push_back(ch);
    }
    return std::nullopt;
}

std::optional<std::string> jsonDecodeQuotedRange(const std::string &json, size_t begin, size_t end)
{
    if (begin >= end || end > json.size() || json[begin] != '"')
        return std::nullopt;

    std::string out;
    bool escape = false;
    for (size_t pos = begin + 1; pos < end; ++pos)
    {
        const char ch = json[pos];
        if (escape)
        {
            out.push_back(ch);
            escape = false;
            continue;
        }
        if (ch == '\\')
        {
            escape = true;
            continue;
        }
        if (ch == '"')
            return out;
        out.push_back(ch);
    }
    return std::nullopt;
}

size_t jsonValueEnd(const std::string &json, size_t valueBegin)
{
    if (valueBegin >= json.size())
        return std::string::npos;

    const char opener = json[valueBegin];
    const char closer = opener == '{' ? '}' : (opener == '[' ? ']' : '\0');
    if (closer == '\0')
        return std::string::npos;

    int depth = 0;
    bool inString = false;
    bool escape = false;
    for (size_t pos = valueBegin; pos < json.size(); ++pos)
    {
        const char ch = json[pos];
        if (escape)
        {
            escape = false;
            continue;
        }
        if (inString)
        {
            if (ch == '\\')
                escape = true;
            else if (ch == '"')
                inString = false;
            continue;
        }
        if (ch == '"')
        {
            inString = true;
            continue;
        }
        if (ch == opener)
            ++depth;
        else if (ch == closer && --depth == 0)
            return pos + 1;
    }
    return std::string::npos;
}

std::optional<std::pair<size_t, size_t>> jsonTopLevelValueRange(const std::string &json, const std::string &key)
{
    const std::string marker = "\"" + key + "\"";
    int depth = 0;
    bool inString = false;
    bool escape = false;
    for (size_t pos = 0; pos < json.size(); ++pos)
    {
        const char ch = json[pos];
        if (escape)
        {
            escape = false;
            continue;
        }
        if (inString)
        {
            if (ch == '\\')
                escape = true;
            else if (ch == '"')
                inString = false;
            continue;
        }
        if (ch == '"')
        {
            if (depth == 1 && json.compare(pos, marker.size(), marker) == 0)
            {
                size_t colon = json.find(':', pos + marker.size());
                if (colon == std::string::npos)
                    return std::nullopt;
                size_t valueBegin = colon + 1;
                while (valueBegin < json.size() && std::isspace(static_cast<unsigned char>(json[valueBegin])))
                    ++valueBegin;
                if (valueBegin >= json.size())
                    return std::nullopt;
                if (json[valueBegin] == '"')
                {
                    size_t valueEnd = valueBegin + 1;
                    bool valueEscape = false;
                    for (; valueEnd < json.size(); ++valueEnd)
                    {
                        if (valueEscape)
                            valueEscape = false;
                        else if (json[valueEnd] == '\\')
                            valueEscape = true;
                        else if (json[valueEnd] == '"')
                            return std::make_pair(valueBegin, valueEnd + 1);
                    }
                    return std::nullopt;
                }
                const size_t valueEnd = jsonValueEnd(json, valueBegin);
                if (valueEnd == std::string::npos)
                    return std::nullopt;
                return std::make_pair(valueBegin, valueEnd);
            }
            inString = true;
            continue;
        }
        if (ch == '{' || ch == '[')
            ++depth;
        else if (ch == '}' || ch == ']')
            --depth;
    }
    return std::nullopt;
}

std::optional<std::string> jsonTopLevelStringValue(const std::string &json, const std::string &key)
{
    const auto range = jsonTopLevelValueRange(json, key);
    if (!range || range->first >= json.size() || json[range->first] != '"')
        return std::nullopt;
    return jsonDecodeQuotedRange(json, range->first, range->second);
}

std::optional<std::string> spotifyTrackArtistName(const std::string &json)
{
    const auto range = jsonTopLevelValueRange(json, "artists");
    if (!range)
        return std::nullopt;
    return jsonStringValueInRange(json, "name", range->first, range->second);
}

std::optional<std::string> spotifyTrackAlbumName(const std::string &json)
{
    const auto range = jsonTopLevelValueRange(json, "album");
    if (!range)
        return std::nullopt;
    return jsonStringValueInRange(json, "name", range->first, range->second);
}

std::optional<std::string> spotifyTrackAlbumImageUrl(const std::string &json)
{
    const auto albumRange = jsonTopLevelValueRange(json, "album");
    if (!albumRange)
        return std::nullopt;

    const std::string imagesMarker = "\"images\"";
    size_t imagesPos = json.find(imagesMarker, albumRange->first);
    if (imagesPos == std::string::npos || imagesPos >= albumRange->second)
        return std::nullopt;

    size_t colon = json.find(':', imagesPos + imagesMarker.size());
    if (colon == std::string::npos || colon >= albumRange->second)
        return std::nullopt;

    size_t valueBegin = colon + 1;
    while (valueBegin < albumRange->second && std::isspace(static_cast<unsigned char>(json[valueBegin])))
        ++valueBegin;
    if (valueBegin >= albumRange->second || json[valueBegin] != '[')
        return std::nullopt;

    const size_t imagesEnd = jsonValueEnd(json, valueBegin);
    if (imagesEnd == std::string::npos || imagesEnd > albumRange->second)
        return std::nullopt;

    return jsonStringValueInRange(json, "url", valueBegin, imagesEnd);
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

std::optional<std::string> playlistIdFromUri(const std::string &spotifyPlaylistUri)
{
    const std::string prefix = "spotify:playlist:";
    if (spotifyPlaylistUri.rfind(prefix, 0) != 0 || spotifyPlaylistUri.size() <= prefix.size())
        return std::nullopt;
    return spotifyPlaylistUri.substr(prefix.size());
}

std::optional<std::string> albumIdFromUri(const std::string &spotifyAlbumUri)
{
    const std::string prefix = "spotify:album:";
    if (spotifyAlbumUri.rfind(prefix, 0) != 0 || spotifyAlbumUri.size() <= prefix.size())
        return std::nullopt;
    return spotifyAlbumUri.substr(prefix.size());
}
} // namespace

SpotifyResult SpotifyApiClient::play(const std::string &spotifyUri, double positionSeconds)
{
    return play(spotifyUri, positionSeconds, true);
}

SpotifyResult SpotifyApiClient::play(const std::string &spotifyUri, double positionSeconds, bool allowMuteOnSync)
{
    const int posMs = static_cast<int>(positionSeconds * 1000.0);
    std::wstring url = playbackUrl(L"/play");
    pfc::string8 device = g_cfg_default_device_id.get();
    if (!device.is_empty())
        url += L"?device_id=" + std::wstring(device.c_str(), device.c_str() + std::strlen(device.c_str()));

    if (allowMuteOnSync && g_cfg_mute_on_sync.get())
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
    return playAlbum(spotifyAlbumUri, zeroBasedOffset, positionSeconds, true);
}

SpotifyResult SpotifyApiClient::playAlbum(const std::string &spotifyAlbumUri, int zeroBasedOffset, double positionSeconds, bool allowMuteOnSync)
{
    if (zeroBasedOffset < 0)
        zeroBasedOffset = 0;
    const int posMs = static_cast<int>(positionSeconds * 1000.0);
    std::wstring url = playbackUrl(L"/play");
    pfc::string8 device = g_cfg_default_device_id.get();
    if (!device.is_empty())
        url += L"?device_id=" + std::wstring(device.c_str(), device.c_str() + std::strlen(device.c_str()));

    if (allowMuteOnSync && g_cfg_mute_on_sync.get())
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
    const auto tracks = searchTracks(query, 1);
    if (tracks.empty())
        return std::nullopt;
    return tracks.front();
}

std::vector<std::string> SpotifyApiClient::searchTracks(const std::string &query, int limit)
{
    const std::string encoded = urlEncode(query);
    if (limit < 1)
        limit = 1;
    if (limit > 10)
        limit = 10;
    const std::wstring url = L"https://api.spotify.com/v1/search?type=track&limit=" + std::to_wstring(limit) + L"&q=" +
                             std::wstring(encoded.begin(), encoded.end());
    const auto response = callSpotifyGet(url);
    if (!response || response->status < 200 || response->status >= 300)
        return {};
    return spotifyUris(response->body, "spotify:track:");
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
    if (const auto title = jsonTopLevelStringValue(response->body, "name"))
        info.title = *title;
    if (const auto artist = spotifyTrackArtistName(response->body))
        info.artist = *artist;
    if (const auto album = spotifyTrackAlbumName(response->body))
        info.album = *album;
    if (const auto imageUrl = spotifyTrackAlbumImageUrl(response->body))
        info.albumImageUrl = *imageUrl;
    if (const auto duration = jsonIntValueLocal(response->body, "duration_ms"))
        info.durationMs = *duration;
    return info;
}

std::optional<std::string> SpotifyApiClient::getAlbumTrackUri(const std::string &spotifyAlbumUri, int zeroBasedOffset)
{
    const auto id = albumIdFromUri(spotifyAlbumUri);
    if (!id)
        return std::nullopt;
    if (zeroBasedOffset < 0)
        zeroBasedOffset = 0;

    const int limit = 50;
    const int offset = (zeroBasedOffset / limit) * limit;
    const std::wstring url = L"https://api.spotify.com/v1/albums/" + std::wstring(id->begin(), id->end()) +
                             L"/tracks?limit=" + std::to_wstring(limit) + L"&offset=" + std::to_wstring(offset);
    const auto response = callSpotifyGet(url);
    if (!response || response->status < 200 || response->status >= 300)
        return std::nullopt;

    const auto uris = spotifyUris(response->body, "spotify:track:");
    const size_t localIndex = static_cast<size_t>(zeroBasedOffset - offset);
    if (localIndex >= uris.size())
        return std::nullopt;
    return uris[localIndex];
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

std::vector<std::string> SpotifyApiClient::getQueueTrackUris()
{
    const auto response = callSpotifyGet(L"https://api.spotify.com/v1/me/player/queue");
    if (!response || response->status < 200 || response->status >= 300)
        return {};
    return spotifyUris(response->body, "spotify:track:");
}

std::vector<std::string> SpotifyApiClient::getPlaylistTrackUris(const std::string &spotifyPlaylistUri)
{
    const auto id = playlistIdFromUri(spotifyPlaylistUri);
    if (!id)
        return {};

    std::vector<std::string> out;
    std::string next = "https://api.spotify.com/v1/playlists/" + *id + "/tracks?fields=items(track(uri)),next&limit=100";
    for (int page = 0; page < 20 && !next.empty(); ++page)
    {
        const std::wstring url(next.begin(), next.end());
        const auto response = callSpotifyGet(url);
        if (!response || response->status < 200 || response->status >= 300)
            break;

        const auto uris = spotifyUris(response->body, "spotify:track:");
        for (const auto &uri : uris)
        {
            if (std::find(out.begin(), out.end(), uri) == out.end())
                out.push_back(uri);
        }

        const auto nextUrl = jsonStringValue(response->body, "next");
        next = nextUrl ? *nextUrl : "";
        if (next == "null")
            next.clear();
    }
    return out;
}
} // namespace fsl
