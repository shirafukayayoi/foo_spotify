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
    int durationMs = 0;
};

class SpotifyApiClient
{
public:
    SpotifyResult play(const std::string &spotifyUri, double positionSeconds);
    SpotifyResult playAlbum(const std::string &spotifyAlbumUri, int zeroBasedOffset, double positionSeconds);
    SpotifyResult pause();
    SpotifyResult seek(double positionSeconds);
    SpotifyResult setVolume(int volumePercent);
    std::optional<std::string> searchTrack(const std::string &query);
    std::optional<std::string> searchAlbum(const std::string &query);
    std::optional<SpotifyTrackInfo> getTrackInfo(const std::string &spotifyTrackUri);
};
} // namespace fsl
