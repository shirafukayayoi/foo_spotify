#include "stdafx.h"
#include "metadata.h"

namespace fsl
{
namespace
{
std::string getMeta(const file_info &info, const char *name)
{
    const char *value = info.meta_get(name, 0);
    return value ? value : "";
}

std::uint32_t fnv1a(const std::string &text, std::uint32_t seed)
{
    std::uint32_t hash = seed;
    for (unsigned char ch : text)
    {
        hash ^= ch;
        hash *= 16777619u;
    }
    return hash;
}

std::string pseudoSha1(const std::string &text)
{
    // Windows CryptoAPI への依存を増やさず、安定キーとして 160bit 風の値を作る。
    const std::uint32_t h1 = fnv1a(text, 2166136261u);
    const std::uint32_t h2 = fnv1a(text, h1 ^ 0x9e3779b9u);
    const std::uint32_t h3 = fnv1a(text, h2 ^ 0x85ebca6bu);
    const std::uint32_t h4 = fnv1a(text, h3 ^ 0xc2b2ae35u);
    const std::uint32_t h5 = fnv1a(text, h4 ^ 0x27d4eb2fu);
    char buf[41] = {};
    std::snprintf(buf, sizeof(buf), "%08x%08x%08x%08x%08x", h1, h2, h3, h4, h5);
    return buf;
}
} // namespace

TrackMetadata readTrackMetadata(const metadb_handle_ptr &track)
{
    TrackMetadata metadata;
    if (track.is_empty())
        return metadata;

    metadata.path = track->get_path();
    file_info_impl info;
    track->get_info(info);
    metadata.title = getMeta(info, "TITLE");
    metadata.artist = getMeta(info, "ARTIST");
    metadata.album = getMeta(info, "ALBUM");
    metadata.albumArtist = getMeta(info, "ALBUM ARTIST");
    metadata.date = getMeta(info, "DATE");
    metadata.lengthSeconds = info.get_length();
    metadata.fileSize = static_cast<std::uint64_t>(track->get_filesize());
    return metadata;
}

std::string makeLocalHash(const TrackMetadata &metadata)
{
    std::ostringstream stream;
    stream << metadata.path << '\n' << metadata.fileSize << '\n' << metadata.title << '\n' << metadata.artist << '\n'
           << metadata.album << '\n' << metadata.date;
    return pseudoSha1(stream.str());
}

std::string makeAlbumId(const TrackMetadata &metadata)
{
    const std::string albumArtist = metadata.albumArtist.empty() ? metadata.artist : metadata.albumArtist;
    return pseudoSha1(albumArtist + "\n" + metadata.album + "\n" + metadata.date);
}

std::string makeSearchQuery(const TrackMetadata &metadata)
{
    std::ostringstream stream;
    if (!metadata.artist.empty())
        stream << "artist:\"" << metadata.artist << "\" ";
    if (!metadata.title.empty())
        stream << "track:\"" << metadata.title << "\" ";
    if (!metadata.album.empty())
        stream << "album:\"" << metadata.album << "\"";
    return stream.str();
}
} // namespace fsl
