#pragma once

namespace fsl
{
struct TrackMetadata
{
    std::string path;
    std::string title;
    std::string artist;
    std::string album;
    std::string albumArtist;
    std::string date;
    int trackNumber = 0;
    int discNumber = 1;
    double lengthSeconds = 0.0;
    std::uint64_t fileSize = 0;
};

TrackMetadata readTrackMetadata(const metadb_handle_ptr &track);
std::string makeLocalHash(const TrackMetadata &metadata);
std::string makeAlbumId(const TrackMetadata &metadata);
std::string makeSearchQuery(const TrackMetadata &metadata);
std::string makeAlbumSearchQuery(const TrackMetadata &metadata);
} // namespace fsl
