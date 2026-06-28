#include "stdafx.h"
#include "mapping_manager.h"
#include "metadata.h"
#include "settings.h"
#include "spotify_follow.h"
#include "spotify_api_client.h"

namespace fsl
{
namespace
{
std::mutex g_suppressMutex;
std::chrono::steady_clock::time_point g_suppressUntil{};
std::chrono::steady_clock::time_point g_controlSuppressUntil{};
std::mutex g_managedRemovalMutex;
bool g_suppressNextManagedRemoval = false;
std::mutex g_workerMutex;
std::thread g_workerThread;
bool g_workerStop = false;
std::mutex g_followStateMutex;
bool g_followPrimed = false;
std::string g_lastFollowedUri;
std::set<std::string> g_seenQueueUris;
std::map<std::string, std::chrono::steady_clock::time_point> g_suppressedFollowUris;

template <typename PlaylistManagerPtr>
std::optional<t_size> findPlaylistByName(PlaylistManagerPtr &playlists, const char *playlistName)
{
    const t_size count = playlists->get_playlist_count();
    for (t_size index = 0; index < count; ++index)
    {
        pfc::string8 name;
        if (playlists->playlist_get_name(index, name) && std::strcmp(name.c_str(), playlistName) == 0)
            return index;
    }
    return std::nullopt;
}

template <typename PlaylistManagerPtr>
t_size getOrCreateSpotifyPlaylist(PlaylistManagerPtr &playlists)
{
    if (const auto existing = findPlaylistByName(playlists, "Spotify Playlist"))
        return *existing;
    return playlists->create_playlist("Spotify Playlist", SIZE_MAX, SIZE_MAX);
}

template <typename PlaylistManagerPtr>
bool findSpotifyUriInPlaylist(PlaylistManagerPtr &playlists, t_size playlist, const std::string &uri, t_size &index)
{
    const t_size count = playlists->playlist_get_item_count(playlist);
    for (t_size i = 0; i < count; ++i)
    {
        metadb_handle_ptr handle;
        if (playlists->playlist_get_item_handle(handle, playlist, i) && std::strcmp(handle->get_path(), uri.c_str()) == 0)
        {
            index = i;
            return true;
        }
    }
    return false;
}

template <typename PlaylistManagerPtr>
bool findPathInPlaylist(PlaylistManagerPtr &playlists, t_size playlist, const std::string &path, t_size &index)
{
    const t_size count = playlists->playlist_get_item_count(playlist);
    for (t_size i = 0; i < count; ++i)
    {
        metadb_handle_ptr handle;
        if (playlists->playlist_get_item_handle(handle, playlist, i) && std::strcmp(handle->get_path(), path.c_str()) == 0)
        {
            index = i;
            return true;
        }
    }
    return false;
}

bool isLocalFilesystemPath(const std::string &path)
{
    if (path.empty() || path.rfind("spotify:", 0) == 0)
        return false;
    try
    {
        return !filesystem::g_is_remote_or_unrecognized(path.c_str());
    }
    catch (...)
    {
        return false;
    }
}

std::optional<std::string> mappedSpotifyUri(const TrackMetadata &metadata)
{
    return MappingManager::instance().getTrackMapping(makeLocalHash(metadata));
}

std::optional<std::string> findLocalPathForSpotifyUriInList(const std::string &spotifyTrackUri, metadb_handle_list_cref handles)
{
    for (t_size index = 0; index < handles.get_count(); ++index)
    {
        const auto metadata = readTrackMetadata(handles.get_item(index));
        if (!isLocalFilesystemPath(metadata.path))
            continue;
        const auto mapped = mappedSpotifyUri(metadata);
        if (mapped && *mapped == spotifyTrackUri)
            return metadata.path;
    }
    return std::nullopt;
}

std::optional<std::string> findLocalPathForSpotifyUri(const std::string &spotifyTrackUri)
{
    auto library = library_manager::get();
    if (library->is_library_enabled())
    {
        metadb_handle_list handles;
        library->get_all_items(handles);
        if (const auto path = findLocalPathForSpotifyUriInList(spotifyTrackUri, handles))
            return path;
    }

    auto playlists = static_api_ptr_t<playlist_manager>();
    const t_size activePlaylist = playlists->get_active_playlist();
    if (activePlaylist != SIZE_MAX)
    {
        metadb_handle_list handles;
        const t_size count = playlists->playlist_get_item_count(activePlaylist);
        for (t_size index = 0; index < count; ++index)
        {
            metadb_handle_ptr handle;
            if (playlists->playlist_get_item_handle(handle, activePlaylist, index))
                handles.add_item(handle);
        }
        if (const auto path = findLocalPathForSpotifyUriInList(spotifyTrackUri, handles))
            return path;
    }

    return std::nullopt;
}

int foobarVolumePercent()
{
    float volumeDb = 0.0f;
    try
    {
        volumeDb = static_api_ptr_t<play_control>()->get_volume();
    }
    catch (...)
    {
        return 100;
    }

    if (volumeDb <= play_control::volume_mute)
        return 0;
    if (volumeDb >= 0.0f)
        return 100;

    const double linear = std::pow(10.0, static_cast<double>(volumeDb) / 20.0);
    return pfc::clip_t<int>(static_cast<int>(std::lround(linear * 100.0)), 0, 100);
}

std::string playbackLocationForSpotifyUri(const std::string &spotifyTrackUri)
{
    if (const auto localPath = findLocalPathForSpotifyUri(spotifyTrackUri))
        return *localPath;
    return spotifyTrackUri;
}

void suppressSpotifyControlSyncForFollow(std::chrono::milliseconds duration)
{
    std::lock_guard<std::mutex> lock(g_suppressMutex);
    g_controlSuppressUntil = std::chrono::steady_clock::now() + duration;
}

class StartSpotifyTrackCallback : public main_thread_callback
{
public:
    StartSpotifyTrackCallback(std::string uri, double positionSeconds, bool externalDevicePlayback)
        : m_uri(std::move(uri)), m_positionSeconds(positionSeconds), m_externalDevicePlayback(externalDevicePlayback)
    {
    }

    void callback_run() override
    {
        if (m_uri.empty())
            return;

        const std::string location = playbackLocationForSpotifyUri(m_uri);
        const bool usingLocalFile = location != m_uri;
        auto playlists = static_api_ptr_t<playlist_manager>();
        const t_size playlist = getOrCreateSpotifyPlaylist(playlists);
        if (playlist == SIZE_MAX)
        {
            FB2K_console_formatter() << "foo_spotify_linker: Spotify Playlist を作成できません。";
            return;
        }
        playlists->set_active_playlist(playlist);

        t_size targetIndex = SIZE_MAX;
        const bool alreadyInPlaylist = usingLocalFile
                                           ? findPathInPlaylist(playlists, playlist, location, targetIndex)
                                           : findSpotifyUriInPlaylist(playlists, playlist, location, targetIndex);
        if (!alreadyInPlaylist)
        {
            targetIndex = playlists->playlist_get_item_count(playlist);
            const char *path = location.c_str();
            if (!playlists->playlist_insert_locations(playlist, targetIndex, pfc::list_single_ref_t<const char *>(path), true, core_api::get_main_window()))
            {
                FB2K_console_formatter() << "foo_spotify_linker: Spotify follow item を追加できません: " << location.c_str();
                return;
            }
        }
        playlists->playlist_set_focus_item(playlist, targetIndex);
        playlists->playlist_ensure_visible(playlist, targetIndex);

        suppressVirtualSpotifyPlaybackFor(std::chrono::seconds(5));
        if (m_externalDevicePlayback)
            suppressSpotifyControlSyncForFollow(std::chrono::seconds(10));
        if (usingLocalFile)
        {
            if (!m_externalDevicePlayback)
                SpotifyApiClient().setVolume(0);
            if (m_externalDevicePlayback)
            {
                static_api_ptr_t<play_control>()->set_volume(play_control::volume_mute);
                FB2K_console_formatter() << "foo_spotify_linker: 外部Spotifyデバイス由来のローカル再生のため fb2k 音量を 0 にしました。";
            }
        }
        else
        {
            if (!m_externalDevicePlayback)
                SpotifyApiClient().setVolume(foobarVolumePercent());
        }
        playlists->playlist_execute_default_action(playlist, targetIndex);
        if (m_positionSeconds > 1.0)
            static_api_ptr_t<play_control>()->playback_seek(m_positionSeconds);
    }

private:
    std::string m_uri;
    double m_positionSeconds = 0.0;
    bool m_externalDevicePlayback = false;
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
        const t_size playlist = getOrCreateSpotifyPlaylist(playlists);
        if (playlist == SIZE_MAX)
            return;
        playlists->set_active_playlist(playlist);

        t_size insertAt = playlists->playlist_get_item_count(playlist);
        for (const std::string &uriText : m_uris)
        {
            const std::string location = playbackLocationForSpotifyUri(uriText);
            t_size existing = SIZE_MAX;
            if (findPathInPlaylist(playlists, playlist, location, existing))
                continue;

            const char *path = location.c_str();
            if (playlists->playlist_insert_locations(playlist, insertAt, pfc::list_single_ref_t<const char *>(path), false, core_api::get_main_window()))
                ++insertAt;
        }
    }

private:
    std::vector<std::string> m_uris;
};

std::vector<std::string> firstUnseenQueueUri(const std::vector<std::string> &uris)
{
    std::lock_guard<std::mutex> lock(g_followStateMutex);
    for (const std::string &uri : uris)
    {
        if (g_seenQueueUris.insert(uri).second)
            return {uri};
    }
    return {};
}

bool primeFollowState(const std::optional<SpotifyPlaybackInfo> &playback)
{
    std::lock_guard<std::mutex> lock(g_followStateMutex);
    if (g_followPrimed)
        return false;

    if (playback && !playback->trackUri.empty())
    {
        g_lastFollowedUri = playback->trackUri;
        g_seenQueueUris.insert(playback->trackUri);
    }
    g_followPrimed = true;
    return true;
}

bool markFollowedPlaybackUri(const std::string &uri)
{
    std::lock_guard<std::mutex> lock(g_followStateMutex);
    g_seenQueueUris.insert(uri);
    const auto suppressed = g_suppressedFollowUris.find(uri);
    if (suppressed != g_suppressedFollowUris.end())
    {
        if (std::chrono::steady_clock::now() < suppressed->second)
        {
            g_lastFollowedUri = uri;
            return false;
        }
        g_suppressedFollowUris.erase(suppressed);
    }
    if (uri == g_lastFollowedUri)
        return false;
    g_lastFollowedUri = uri;
    return true;
}

bool isExternalPlaybackDevice(const SpotifyPlaybackInfo &playback)
{
    pfc::string8 configuredDeviceId = g_cfg_default_device_id.get();
    if (!configuredDeviceId.is_empty() && !playback.deviceId.empty() && playback.deviceId != configuredDeviceId.c_str())
        return true;

    if (!playback.deviceType.empty() && _stricmp(playback.deviceType.c_str(), "Computer") != 0)
        return true;

    return false;
}

void resetFollowPrime()
{
    std::lock_guard<std::mutex> lock(g_followStateMutex);
    g_followPrimed = false;
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

bool shouldSuppressSpotifyControls()
{
    std::lock_guard<std::mutex> lock(g_suppressMutex);
    return std::chrono::steady_clock::now() < g_controlSuppressUntil;
}

void suppressSpotifyControlsFor(std::chrono::milliseconds duration)
{
    std::lock_guard<std::mutex> lock(g_suppressMutex);
    g_controlSuppressUntil = std::chrono::steady_clock::now() + duration;
}

void suppressFollowedSpotifyTrack(const std::string &spotifyTrackUri, std::chrono::milliseconds duration)
{
    if (spotifyTrackUri.rfind("spotify:track:", 0) != 0)
        return;

    std::lock_guard<std::mutex> lock(g_followStateMutex);
    g_suppressedFollowUris[spotifyTrackUri] = std::chrono::steady_clock::now() + duration;
    g_seenQueueUris.insert(spotifyTrackUri);
}

void suppressNextManagedPlaylistRemoval()
{
    std::lock_guard<std::mutex> lock(g_managedRemovalMutex);
    g_suppressNextManagedRemoval = true;
}

bool consumeSuppressNextManagedPlaylistRemoval()
{
    std::lock_guard<std::mutex> lock(g_managedRemovalMutex);
    const bool suppressed = g_suppressNextManagedRemoval;
    g_suppressNextManagedRemoval = false;
    return suppressed;
}

void startFollowedSpotifyTrackInFoobar(const std::string &spotifyTrackUri, double positionSeconds, bool externalDevicePlayback)
{
    main_thread_callback_manager::get()->add_callback(new service_impl_t<StartSpotifyTrackCallback>(spotifyTrackUri, positionSeconds, externalDevicePlayback));
}

void addSpotifyQueueTracksInFoobar(const std::vector<std::string> &spotifyTrackUris)
{
    if (!spotifyTrackUris.empty())
        main_thread_callback_manager::get()->add_callback(new service_impl_t<AddSpotifyQueueItemsCallback>(spotifyTrackUris));
}

void requestNextSpotifyQueueTrackInFoobar()
{
    std::thread([] {
        SpotifyApiClient client;
        addSpotifyQueueTracksInFoobar(firstUnseenQueueUri(client.getQueueTrackUris()));
    }).detach();
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
                if (primeFollowState(playback))
                {
                    const int requestedInterval = static_cast<int>(g_cfg_polling_interval_ms.get());
                    const int interval = requestedInterval < 1000 ? 1000 : requestedInterval;
                    std::this_thread::sleep_for(std::chrono::milliseconds(interval));
                    continue;
                }

                if (playback && playback->isPlaying && !playback->trackUri.empty())
                {
                    const bool externalDevicePlayback = isExternalPlaybackDevice(*playback);
                    if (externalDevicePlayback)
                        suppressSpotifyControlsFor(std::chrono::seconds(10));

                    if (markFollowedPlaybackUri(playback->trackUri))
                        startFollowedSpotifyTrackInFoobar(playback->trackUri, static_cast<double>(playback->progressMs) / 1000.0, externalDevicePlayback);
                }

            }
            else
            {
                resetFollowPrime();
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
