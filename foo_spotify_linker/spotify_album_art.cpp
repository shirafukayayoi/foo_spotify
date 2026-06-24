#include "stdafx.h"
#include "http_client.h"
#include "spotify_api_client.h"

#include <SDK/album_art_helpers.h>

#include <map>

namespace fsl
{
namespace
{
bool isSpotifyTrackPath(const char *path)
{
    return path != nullptr && std::strncmp(path, "spotify:track:", 14) == 0 && std::strlen(path) > 14;
}

std::wstring widenUtf8(const std::string &value)
{
    if (value.empty())
        return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if (len <= 1)
        return {};
    std::wstring out(static_cast<size_t>(len - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, out.data(), len);
    return out;
}

bool looksLikeImage(const std::string &body)
{
    if (body.size() < 12)
        return false;

    const unsigned char *data = reinterpret_cast<const unsigned char *>(body.data());
    const bool jpeg = data[0] == 0xff && data[1] == 0xd8 && data[2] == 0xff;
    const bool png = data[0] == 0x89 && data[1] == 0x50 && data[2] == 0x4e && data[3] == 0x47;
    const bool webp = body.compare(0, 4, "RIFF") == 0 && body.compare(8, 4, "WEBP") == 0;
    return jpeg || png || webp;
}

class SpotifyAlbumArtCache
{
public:
    album_art_data_ptr get(const std::string &uri, abort_callback &abort)
    {
        abort.check();
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            const auto found = m_artByUri.find(uri);
            if (found != m_artByUri.end())
                return found->second;
            if (m_missingUris.find(uri) != m_missingUris.end())
                throw exception_album_art_not_found();
        }

        const auto info = SpotifyApiClient().getTrackInfo(uri);
        abort.check();
        if (!info || info->albumImageUrl.empty())
        {
            markMissing(uri);
            throw exception_album_art_not_found();
        }

        album_art_data_ptr art;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            const auto found = m_artByUrl.find(info->albumImageUrl);
            if (found != m_artByUrl.end())
            {
                art = found->second;
                m_artByUri[uri] = art;
                return art;
            }
        }

        const auto response = httpRequest(L"GET", widenUtf8(info->albumImageUrl), {}, "");
        abort.check();
        if (!response || response->status < 200 || response->status >= 300 || !looksLikeImage(response->body))
        {
            markMissing(uri);
            throw exception_album_art_not_found();
        }

        art = album_art_data_impl::g_create(response->body.data(), response->body.size());
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_artByUri[uri] = art;
            m_artByUrl[info->albumImageUrl] = art;
            while (m_artByUri.size() > 256)
                m_artByUri.erase(m_artByUri.begin());
            while (m_artByUrl.size() > 256)
                m_artByUrl.erase(m_artByUrl.begin());
        }
        return art;
    }

private:
    void markMissing(const std::string &uri)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_missingUris.insert(uri);
        while (m_missingUris.size() > 256)
            m_missingUris.erase(m_missingUris.begin());
    }

    std::mutex m_mutex;
    std::map<std::string, album_art_data_ptr> m_artByUri;
    std::map<std::string, album_art_data_ptr> m_artByUrl;
    std::set<std::string> m_missingUris;
};

SpotifyAlbumArtCache &albumArtCache()
{
    static SpotifyAlbumArtCache cache;
    return cache;
}

class SpotifyAlbumArtInstance : public album_art_extractor_instance
{
public:
    explicit SpotifyAlbumArtInstance(const char *path) : m_uri(path ? path : "")
    {
    }

    album_art_data_ptr query(const GUID &what, abort_callback &abort) override
    {
        abort.check();
        if (what != album_art_ids::cover_front)
            throw exception_album_art_not_found();
        if (!isSpotifyTrackPath(m_uri.c_str()))
            throw exception_album_art_not_found();
        return albumArtCache().get(m_uri, abort);
    }

private:
    std::string m_uri;
};

class SpotifyAlbumArtExtractor : public album_art_extractor_v2
{
public:
    bool is_our_path(const char *path, const char *) override
    {
        return isSpotifyTrackPath(path);
    }

    album_art_extractor_instance_ptr open(file_ptr, const char *path, abort_callback &abort) override
    {
        abort.check();
        if (!isSpotifyTrackPath(path))
            throw exception_album_art_unsupported_format();
        return new service_impl_t<SpotifyAlbumArtInstance>(path);
    }

    GUID get_guid() override
    {
        static constexpr GUID guid = {0x3da74c52, 0x9ec0, 0x44c9, {0x9a, 0xf9, 0x23, 0x28, 0x64, 0x5d, 0xb3, 0x18}};
        return guid;
    }
};

static service_factory_single_t<SpotifyAlbumArtExtractor> g_spotifyAlbumArtExtractorFactory;
} // namespace
} // namespace fsl
