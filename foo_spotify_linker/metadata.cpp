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

int parseLeadingInt(const std::string &value, int fallback)
{
    int result = 0;
    bool any = false;
    for (char ch : value)
    {
        if (!std::isdigit(static_cast<unsigned char>(ch)))
            break;
        result = result * 10 + (ch - '0');
        any = true;
    }
    return any ? result : fallback;
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

std::string trimAscii(std::string value)
{
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
        value.erase(value.begin());
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
        value.pop_back();
    return value;
}

std::string lowerAscii(std::string value)
{
    for (char &ch : value)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return value;
}

bool isDigits(const std::string &value)
{
    return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char ch) { return std::isdigit(ch); });
}

std::vector<std::string> splitAsciiWords(const std::string &value)
{
    std::vector<std::string> words;
    std::istringstream stream(value);
    std::string word;
    while (stream >> word)
        words.push_back(word);
    return words;
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
    metadata.trackNumber = parseLeadingInt(getMeta(info, "TRACKNUMBER"), 0);
    metadata.discNumber = parseLeadingInt(getMeta(info, "DISCNUMBER"), 1);
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

std::string cleanAlbumTitleForSpotify(const std::string &album)
{
    std::vector<std::string> words = splitAsciiWords(trimAscii(album));
    while (!words.empty())
    {
        const std::string word = lowerAscii(words.back());
        if (isDigits(word) && words.size() >= 2)
        {
            const std::string previous = lowerAscii(words[words.size() - 2]);
            if (previous == "cd" || previous == "disc" || previous == "disk")
            {
                words.pop_back();
                words.pop_back();
                continue;
            }
        }
        if ((word.size() > 1 && word[0] == '#' && isDigits(word.substr(1))) ||
            (word.size() > 2 && word.rfind("cd", 0) == 0 && isDigits(word.substr(2))) ||
            (word.size() > 4 && word.rfind("disc", 0) == 0 && isDigits(word.substr(4))) ||
            word == "cd" || word == "disc" || word == "disk")
        {
            words.pop_back();
            continue;
        }
        break;
    }

    std::ostringstream stream;
    for (size_t i = 0; i < words.size(); ++i)
    {
        if (i > 0)
            stream << ' ';
        stream << words[i];
    }
    return stream.str();
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

std::string makeAlbumSearchQuery(const TrackMetadata &metadata)
{
    std::ostringstream stream;
    const std::string albumArtist = metadata.albumArtist.empty() ? metadata.artist : metadata.albumArtist;
    if (!albumArtist.empty())
        stream << "artist:\"" << albumArtist << "\" ";
    if (!metadata.album.empty())
        stream << "album:\"" << metadata.album << "\"";
    return stream.str();
}

std::vector<std::string> makeTrackSearchQueries(const TrackMetadata &metadata)
{
    std::vector<std::string> queries;
    const auto add = [&queries](const std::string &query) {
        if (!query.empty() && std::find(queries.begin(), queries.end(), query) == queries.end())
            queries.push_back(query);
    };

    add(makeSearchQuery(metadata));
    const std::string cleanAlbum = cleanAlbumTitleForSpotify(metadata.album);
    if (!metadata.title.empty() && !cleanAlbum.empty() && cleanAlbum != metadata.album)
    {
        if (!metadata.artist.empty())
            add("artist:\"" + metadata.artist + "\" track:\"" + metadata.title + "\" album:\"" + cleanAlbum + "\"");
        add("track:\"" + metadata.title + "\" album:\"" + cleanAlbum + "\"");
    }
    if (!metadata.title.empty() && !metadata.album.empty())
        add("track:\"" + metadata.title + "\" album:\"" + metadata.album + "\"");
    if (!metadata.title.empty() && !metadata.artist.empty())
        add("track:\"" + metadata.title + "\" artist:\"" + metadata.artist + "\"");
    if (!metadata.title.empty())
        add("track:\"" + metadata.title + "\"");
    return queries;
}

std::vector<std::string> makeAlbumSearchQueries(const TrackMetadata &metadata)
{
    std::vector<std::string> queries;
    const auto add = [&queries](const std::string &query) {
        if (!query.empty() && std::find(queries.begin(), queries.end(), query) == queries.end())
            queries.push_back(query);
    };

    add(makeAlbumSearchQuery(metadata));
    const std::string cleanAlbum = cleanAlbumTitleForSpotify(metadata.album);
    if (!cleanAlbum.empty() && cleanAlbum != metadata.album)
        add("album:\"" + cleanAlbum + "\"");
    if (!metadata.album.empty())
        add("album:\"" + metadata.album + "\"");
    return queries;
}
} // namespace fsl
