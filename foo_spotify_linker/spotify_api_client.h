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
    SpotifyResult pause();
    SpotifyResult seek(double positionSeconds);
    SpotifyResult setVolume(int volumePercent);
};
} // namespace fsl
