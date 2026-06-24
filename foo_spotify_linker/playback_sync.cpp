#include "stdafx.h"
#include "mapping_manager.h"
#include "metadata.h"
#include "settings.h"
#include "spotify_api_client.h"

namespace fsl
{
namespace
{
bool isSpotifyVirtualTrack(const std::string &path)
{
    return path.rfind("spotify:track:", 0) == 0;
}

int foobarVolumeDbToPercent(float volumeDb)
{
    if (volumeDb <= play_control::volume_mute)
        return 0;
    if (volumeDb >= 0.0f)
        return 100;

    const double linear = std::pow(10.0, static_cast<double>(volumeDb) / 20.0);
    return pfc::clip_t<int>(static_cast<int>(std::lround(linear * 100.0)), 0, 100);
}

class LinkerLifecycle : public initquit
{
public:
    void on_init() override
    {
        const std::string path = effectiveDbPath();
        if (MappingManager::instance().open(path))
            FB2K_console_formatter() << "foo_spotify_linker: DB を開きました: " << path.c_str();
    }

    void on_quit() override
    {
        MappingManager::instance().close();
    }
};

class PlaybackSync : public play_callback_static
{
public:
    unsigned get_flags() override
    {
        return flag_on_playback_new_track | flag_on_playback_time | flag_on_playback_stop | flag_on_playback_pause |
               flag_on_playback_seek | flag_on_volume_change;
    }

    void on_playback_new_track(metadb_handle_ptr track) override
    {
        const TrackMetadata metadata = readTrackMetadata(track);
        if (isSpotifyVirtualTrack(metadata.path))
        {
            m_lastSpotifyUri = metadata.path;
            return;
        }

        const auto uri = MappingManager::instance().resolve(metadata);
        if (!uri)
        {
            FB2K_console_formatter() << "foo_spotify_linker: マッピング未登録: " << metadata.artist.c_str() << " - "
                                     << metadata.title.c_str();
            return;
        }

        m_lastSpotifyUri = *uri;
        const int zeroBasedAlbumOffset = metadata.trackNumber > 0 ? metadata.trackNumber - 1 : 0;
        if (uri->rfind("spotify:album:", 0) == 0)
            m_client.playAlbum(*uri, zeroBasedAlbumOffset, 0.0);
        else
            m_client.play(*uri, 0.0);
    }

    void on_playback_stop(play_control::t_stop_reason reason) override
    {
        if (reason == play_control::stop_reason_starting_another)
            return;
        m_client.pause();
        m_lastSpotifyUri.clear();
    }

    void on_playback_pause(bool state) override
    {
        if (state)
            m_client.pause();
    }

    void on_playback_time(double time) override
    {
        if (m_lastSpotifyUri.empty())
            return;
        const int interval = static_cast<int>(g_cfg_polling_interval_ms.get());
        if (interval <= 0)
            return;

        const auto now = std::chrono::steady_clock::now();
        if (now - m_lastSeekCheck < std::chrono::milliseconds(interval))
            return;
        m_lastSeekCheck = now;

        // 実 API 接続後に Spotify 側の現在位置を取得し、500ms 超のズレを補正する。
        (void)time;
    }

    void on_playback_starting(play_control::t_track_command command, bool paused) override
    {
        (void)command;
        (void)paused;
    }

    void on_playback_seek(double time) override
    {
        if (!m_lastSpotifyUri.empty())
            m_client.seek(time);
    }

    void on_playback_edited(metadb_handle_ptr track) override
    {
        (void)track;
    }

    void on_playback_dynamic_info(const file_info &info) override
    {
        (void)info;
    }

    void on_playback_dynamic_info_track(const file_info &info) override
    {
        (void)info;
    }

    void on_volume_change(float newValue) override
    {
        if (isSpotifyVirtualTrack(m_lastSpotifyUri))
            m_client.setVolume(foobarVolumeDbToPercent(newValue));
    }

private:
    SpotifyApiClient m_client;
    std::string m_lastSpotifyUri;
    std::chrono::steady_clock::time_point m_lastSeekCheck{};
};

initquit_factory_t<LinkerLifecycle> g_lifecycle;
play_callback_static_factory_t<PlaybackSync> g_playbackSync;
} // namespace
} // namespace fsl
