#pragma once

namespace fsl
{
struct SpotifyResult
{
    bool ok = false;
    std::string message;
};

struct SpotifyTrackInfo
{
    std::string uri;
    std::string title;
    std::string artist;
    std::string album;
    std::string albumImageUrl;
    int durationMs = 0;
};

struct SpotifyPlaybackInfo
{
    std::string trackUri;
    int progressMs = 0;
    bool isPlaying = false;
};

class SpotifyApiClient
{
public:
    SpotifyResult play(const std::string &spotifyUri, double positionSeconds);
    SpotifyResult playVirtualTrack(const std::string &spotifyTrackUri, double positionSeconds, int volumePercent);
    SpotifyResult playAlbum(const std::string &spotifyAlbumUri, int zeroBasedOffset, double positionSeconds);
    SpotifyResult pause();
    SpotifyResult seek(double positionSeconds);
    SpotifyResult setVolume(int volumePercent);
    std::optional<std::string> searchTrack(const std::string &query);
    std::optional<std::string> searchAlbum(const std::string &query);
    std::optional<SpotifyTrackInfo> getTrackInfo(const std::string &spotifyTrackUri);
    std::optional<SpotifyPlaybackInfo> getCurrentPlayback();
    std::vector<std::string> getQueueTrackUris();
    std::vector<std::string> getPlaylistTrackUris(const std::string &spotifyPlaylistUri);
};
} // namespace fsl
