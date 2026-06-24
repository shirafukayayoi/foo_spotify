#include "stdafx.h"
#include "settings.h"
#include "spotify_follow.h"
#include "spotify_api_client.h"

namespace fsl
{
namespace
{
std::mutex g_suppressMutex;
std::chrono::steady_clock::time_point g_suppressUntil{};
std::mutex g_workerMutex;
std::thread g_workerThread;
bool g_workerStop = false;
std::string g_lastFollowedUri;
std::set<std::string> g_seenQueueUris;

template <typename PlaylistManagerPtr>
bool findSpotifyUriInActivePlaylist(PlaylistManagerPtr &playlists, const std::string &uri, t_size &index)
{
    const t_size count = playlists->activeplaylist_get_item_count();
    for (t_size i = 0; i < count; ++i)
    {
        metadb_handle_ptr handle;
        if (playlists->activeplaylist_get_item_handle(handle, i) && std::strcmp(handle->get_path(), uri.c_str()) == 0)
        {
            index = i;
            return true;
        }
    }
    return false;
}

class StartSpotifyTrackCallback : public main_thread_callback
{
public:
    StartSpotifyTrackCallback(std::string uri, double positionSeconds)
        : m_uri(std::move(uri)), m_positionSeconds(positionSeconds)
    {
    }

    void callback_run() override
    {
        if (m_uri.empty())
            return;

        auto playlists = static_api_ptr_t<playlist_manager>();
        t_size targetIndex = SIZE_MAX;
        if (!findSpotifyUriInActivePlaylist(playlists, m_uri, targetIndex))
        {
            targetIndex = playlists->activeplaylist_get_item_count();
            const char *uri = m_uri.c_str();
            if (!playlists->activeplaylist_insert_locations(targetIndex, pfc::list_single_ref_t<const char *>(uri), true, core_api::get_main_window()))
            {
                FB2K_console_formatter() << "foo_spotify_linker: Spotify follow item を追加できません: " << m_uri.c_str();
                return;
            }
        }
        playlists->activeplaylist_set_focus_item(targetIndex);
        playlists->activeplaylist_ensure_visible(targetIndex);

        suppressVirtualSpotifyPlaybackFor(std::chrono::seconds(5));
        playlists->activeplaylist_execute_default_action(targetIndex);
        if (m_positionSeconds > 1.0)
            static_api_ptr_t<play_control>()->playback_seek(m_positionSeconds);
    }

private:
    std::string m_uri;
    double m_positionSeconds = 0.0;
};

class AddSpotifyQueueItemsCallback : public main_thread_callback
{
public:
    explicit AddSpotifyQueueItemsCallback(std::vector<std::string> uris) : m_uris(std::move(uris)) {}

    void callback_run() override
    {
        if (m_uris.empty())
            return;

        auto playlists = static_api_ptr_t<playlist_manager>();
        t_size insertAt = playlists->activeplaylist_get_item_count();
        for (const std::string &uriText : m_uris)
        {
            t_size existing = SIZE_MAX;
            if (findSpotifyUriInActivePlaylist(playlists, uriText, existing))
                continue;

            const char *uri = uriText.c_str();
            if (playlists->activeplaylist_insert_locations(insertAt, pfc::list_single_ref_t<const char *>(uri), false, core_api::get_main_window()))
                ++insertAt;
        }
    }

private:
    std::vector<std::string> m_uris;
};

std::vector<std::string> unseenQueueUris(const std::vector<std::string> &uris)
{
    std::vector<std::string> out;
    for (const std::string &uri : uris)
    {
        if (g_seenQueueUris.insert(uri).second)
            out.push_back(uri);
    }
    return out;
}
} // namespace

bool shouldSuppressVirtualSpotifyPlayback()
{
    std::lock_guard<std::mutex> lock(g_suppressMutex);
    return std::chrono::steady_clock::now() < g_suppressUntil;
}

void suppressVirtualSpotifyPlaybackFor(std::chrono::milliseconds duration)
{
    std::lock_guard<std::mutex> lock(g_suppressMutex);
    g_suppressUntil = std::chrono::steady_clock::now() + duration;
}

void startFollowedSpotifyTrackInFoobar(const std::string &spotifyTrackUri, double positionSeconds)
{
    main_thread_callback_manager::get()->add_callback(new service_impl_t<StartSpotifyTrackCallback>(spotifyTrackUri, positionSeconds));
}

void addSpotifyQueueTracksInFoobar(const std::vector<std::string> &spotifyTrackUris)
{
    if (!spotifyTrackUris.empty())
        main_thread_callback_manager::get()->add_callback(new service_impl_t<AddSpotifyQueueItemsCallback>(spotifyTrackUris));
}

void startSpotifyFollowWorker()
{
    std::lock_guard<std::mutex> lock(g_workerMutex);
    if (g_workerThread.joinable())
        return;

    g_workerStop = false;
    g_workerThread = std::thread([] {
        SpotifyApiClient client;
        while (true)
        {
            {
                std::lock_guard<std::mutex> lock(g_workerMutex);
                if (g_workerStop)
                    break;
            }

            if (g_cfg_follow_spotify_playback.get())
            {
                const auto playback = client.getCurrentPlayback();
                if (playback && playback->isPlaying && !playback->trackUri.empty())
                {
                    g_seenQueueUris.insert(playback->trackUri);
                    if (playback->trackUri != g_lastFollowedUri)
                    {
                        g_lastFollowedUri = playback->trackUri;
                        startFollowedSpotifyTrackInFoobar(playback->trackUri, static_cast<double>(playback->progressMs) / 1000.0);
                    }
                }

                const auto queueUris = unseenQueueUris(client.getQueueTrackUris());
                addSpotifyQueueTracksInFoobar(queueUris);
            }

            const int requestedInterval = static_cast<int>(g_cfg_polling_interval_ms.get());
            const int interval = requestedInterval < 1000 ? 1000 : requestedInterval;
            std::this_thread::sleep_for(std::chrono::milliseconds(interval));
        }
    });
}

void stopSpotifyFollowWorker()
{
    {
        std::lock_guard<std::mutex> lock(g_workerMutex);
        g_workerStop = true;
    }
    if (g_workerThread.joinable())
        g_workerThread.join();
}
} // namespace fsl
