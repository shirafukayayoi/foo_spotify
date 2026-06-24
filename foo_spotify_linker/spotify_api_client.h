#pragma once

namespace fsl
{
struct SpotifyResult
{
    bool ok = false;
    std::string message;
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
};
} // namespace fsl
