#include "stdafx.h"
#include "spotify_follow.h"
#include "spotify_api_client.h"

#include <SDK/input_impl.h>

namespace fsl
{
namespace
{
constexpr double fallbackLengthSeconds = 180.0;
constexpr unsigned sampleRate = 44100;
constexpr unsigned channels = 2;
constexpr unsigned chunkSamples = 2048;

bool isSpotifyTrackPath(const char *path)
{
    return path != nullptr && std::strncmp(path, "spotify:track:", 14) == 0 && std::strlen(path) > 14;
}

double secondsFromMs(int ms)
{
    return ms > 0 ? static_cast<double>(ms) / 1000.0 : fallbackLengthSeconds;
}

int foobarVolumePercent()
{
    float volumeDb = 0.0f;
    try
    {
        fb2k::inMainThreadSynchronous2([&volumeDb] {
            volumeDb = static_api_ptr_t<play_control>()->get_volume();
        });
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

bool queueReplayCurrentPlaylistItem()
{
    t_size playlist = SIZE_MAX;
    t_size item = SIZE_MAX;
    try
    {
        fb2k::inMainThreadSynchronous2([&playlist, &item] {
            auto playlists = static_api_ptr_t<playlist_manager>();
            if (!playlists->get_playing_item_location(&playlist, &item))
            {
                playlist = SIZE_MAX;
                item = SIZE_MAX;
            }
        });
    }
    catch (...)
    {
        return false;
    }

    if (playlist == SIZE_MAX || item == SIZE_MAX)
        return false;

    fb2k::inMainThread([playlist, item] {
        auto playlists = static_api_ptr_t<playlist_manager>();
        suppressVirtualSpotifyPlaybackFor(std::chrono::seconds(5));
        suppressSpotifyControlsFor(std::chrono::seconds(10));
        playlists->playlist_execute_default_action(playlist, item);
    });
    return true;
}

class SpotifyTrackInput : public input_stubs
{
public:
    void open(service_ptr_t<file> fileHint, const char *path, t_input_open_reason reason, abort_callback &abort)
    {
        (void)fileHint;
        abort.check();
        if (!isSpotifyTrackPath(path))
            throw exception_io_data();
        if (reason == input_open_info_write)
            throw exception_tagging_unsupported();

        m_uri = path;
        if (const auto info = SpotifyApiClient().getTrackInfo(m_uri))
            m_info = *info;
        else
            m_info.uri = m_uri;
        m_length = secondsFromMs(m_info.durationMs);
    }

    void get_info(file_info &info, abort_callback &abort)
    {
        abort.check();
        info.set_length(m_length);
        info.info_set("codec", "Spotify Linker silent proxy");
        info.info_set_int("samplerate", sampleRate);
        info.info_set_int("channels", channels);
        info.info_set("spotify_uri", m_uri.c_str());
        info.info_set("encoding", "silent PCM");

        info.meta_set("TITLE", m_info.title.empty() ? m_uri.c_str() : m_info.title.c_str());
        if (!m_info.artist.empty())
            info.meta_set("ARTIST", m_info.artist.c_str());
        if (!m_info.album.empty())
            info.meta_set("ALBUM", m_info.album.c_str());
    }

    t_filestats2 get_stats2(uint32_t, abort_callback &abort)
    {
        abort.check();
        t_filestats2 stats;
        stats.m_size = 0;
        stats.m_timestamp = filetimestamp_invalid;
        stats.set_attrib(t_filestats2::attr_remote, true);
        stats.set_attrib(t_filestats2::attr_readonly, true);
        return stats;
    }

    void decode_initialize(unsigned flags, abort_callback &abort)
    {
        abort.check();
        m_position = 0.0;
        m_started = false;
        m_endGraceUntil = {};
        m_replayRequested = false;
        if (flags & input_flag_playback)
            startSpotifyPlayback(0.0);
    }

    bool decode_run(audio_chunk &chunk, abort_callback &abort)
    {
        abort.check();
        if (m_position >= m_length)
        {
            if (!waitForSpotifyLoopOrReplay(abort))
                return false;
        }

        const double remaining = m_length - m_position;
        const unsigned samples = static_cast<unsigned>(std::min<double>(chunkSamples, remaining * sampleRate));
        if (samples == 0)
            return false;

        chunk.reset();
        chunk.pad_with_silence_ex(samples, channels, sampleRate);
        m_position += static_cast<double>(samples) / sampleRate;
        return true;
    }

    void decode_seek(double seconds, abort_callback &abort)
    {
        abort.check();
        if (seconds < 0.0)
            seconds = 0.0;
        if (seconds > m_length)
            seconds = m_length;
        m_position = seconds;
        m_endGraceUntil = {};
        m_replayRequested = false;
        if (!shouldSuppressSpotifyControls())
            startSpotifyPlayback(seconds);
    }

    bool decode_can_seek()
    {
        return true;
    }

    void set_pause(bool paused)
    {
        if (shouldSuppressSpotifyControls())
            return;
        if (paused)
            SpotifyApiClient().pause();
        else
            startSpotifyPlayback(m_position);
    }

    void retag(const file_info &, abort_callback &)
    {
        throw exception_tagging_unsupported();
    }

    void remove_tags(abort_callback &)
    {
        throw exception_tagging_unsupported();
    }

    static bool g_is_our_content_type(const char *contentType)
    {
        return contentType != nullptr && std::strcmp(contentType, "x-spotify-track") == 0;
    }

    static bool g_is_our_path(const char *path, const char *)
    {
        return isSpotifyTrackPath(path);
    }

    static GUID g_get_guid()
    {
        static constexpr GUID guid = {0x3da74c52, 0x9ec0, 0x44c9, {0x9a, 0xf9, 0x23, 0x28, 0x64, 0x5d, 0xb3, 0x18}};
        return guid;
    }

    static const char *g_get_name()
    {
        return "Spotify Linker virtual track";
    }

private:
    void startSpotifyPlayback(double positionSeconds)
    {
        if (m_uri.empty())
            return;
        if (shouldSuppressVirtualSpotifyPlayback())
        {
            SpotifyApiClient().setVolume(foobarVolumePercent());
            m_started = true;
            return;
        }
        SpotifyApiClient client;
        const SpotifyResult result = client.playVirtualTrack(m_uri, positionSeconds, foobarVolumePercent());
        if (!result.ok)
            FB2K_console_formatter() << "foo_spotify_linker: Spotify virtual playback failed: " << result.message.c_str();
        m_started = result.ok;
    }

    bool waitForSpotifyLoopOrReplay(abort_callback &abort)
    {
        abort.check();
        if (m_uri.empty() || m_replayRequested)
            return false;

        const auto now = std::chrono::steady_clock::now();
        if (m_endGraceUntil.time_since_epoch().count() == 0)
            m_endGraceUntil = now + std::chrono::seconds(3);
        if (now > m_endGraceUntil)
        {
            m_endGraceUntil = {};
            return false;
        }

        const auto playback = SpotifyApiClient().getCurrentPlayback();
        abort.check();
        if (!playback || !playback->isPlaying || playback->trackUri != m_uri)
            return false;

        if (playback->progressMs >= 0 && playback->progressMs <= 5000)
        {
            m_endGraceUntil = {};
            m_replayRequested = true;
            if (queueReplayCurrentPlaylistItem())
                suppressNextManagedPlaylistRemoval();
            return false;
        }

        m_position = m_length > 0.25 ? m_length - 0.25 : 0.0;
        return true;
    }

    std::string m_uri;
    SpotifyTrackInfo m_info;
    double m_length = fallbackLengthSeconds;
    double m_position = 0.0;
    bool m_started = false;
    bool m_replayRequested = false;
    std::chrono::steady_clock::time_point m_endGraceUntil{};
};

static input_singletrack_factory_t<SpotifyTrackInput> g_spotifyTrackInputFactory;
} // namespace
} // namespace fsl
