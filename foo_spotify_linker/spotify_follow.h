#pragma once

namespace fsl
{
bool shouldSuppressVirtualSpotifyPlayback();
void suppressVirtualSpotifyPlaybackFor(std::chrono::milliseconds duration);
void startFollowedSpotifyTrackInFoobar(const std::string &spotifyTrackUri, double positionSeconds);
void startSpotifyFollowWorker();
void stopSpotifyFollowWorker();
} // namespace fsl
