#include "stdafx.h"
#include "http_client.h"
#include "mapping_manager.h"
#include "metadata.h"
#include "resource.h"
#include "spotify_api_client.h"

namespace fsl
{
namespace
{
static constexpr GUID guid_mainmenu_group_spotify = {0xf7c54c3f, 0x8bc2, 0x4ca2, {0x91, 0xac, 0x0e, 0x8f, 0xd6, 0xf5, 0x46, 0x42}};
static constexpr GUID guid_mainmenu_add_link = {0xc2f2cf2a, 0x33fb, 0x43b5, {0x94, 0xcb, 0x25, 0xcd, 0x64, 0x8a, 0x0e, 0x55}};
static constexpr GUID guid_mainmenu_autolink_library = {0x32af6437, 0x3bfd, 0x46d6, {0x95, 0xda, 0xc4, 0xf8, 0x8d, 0xcd, 0x31, 0x66}};

enum class SpotifyLinkKind
{
    track,
    album,
    playlist,
    socialSession
};

struct SpotifyLink
{
    SpotifyLinkKind kind = SpotifyLinkKind::track;
    std::string uri;
};

struct LibraryAutoLinkStats
{
    size_t total = 0;
    size_t registered = 0;
    size_t alreadyMapped = 0;
    size_t skipped = 0;
    size_t failed = 0;
    size_t candidatesRejected = 0;
};

std::string normalizedText(const std::string &value)
{
    std::string out;
    bool pendingSpace = false;
    for (unsigned char ch : value)
    {
        if (std::isspace(ch))
        {
            pendingSpace = !out.empty();
            continue;
        }
        if (pendingSpace)
        {
            out.push_back(' ');
            pendingSpace = false;
        }
        out.push_back(static_cast<char>(std::tolower(ch)));
    }
    if (!out.empty() && out.back() == ' ')
        out.pop_back();
    return out;
}

bool sameTextStrict(const std::string &left, const std::string &right)
{
    return !left.empty() && normalizedText(left) == normalizedText(right);
}

bool sameAlbumTextStrict(const std::string &left, const std::string &right)
{
    if (sameTextStrict(left, right))
        return true;
    const std::string cleanLeft = cleanAlbumTitleForSpotify(left);
    const std::string cleanRight = cleanAlbumTitleForSpotify(right);
    if (!cleanLeft.empty() && sameTextStrict(cleanLeft, right))
        return true;
    if (!cleanRight.empty() && sameTextStrict(left, cleanRight))
        return true;
    return !cleanLeft.empty() && !cleanRight.empty() && sameTextStrict(cleanLeft, cleanRight);
}

bool isStrictTrackMatch(const TrackMetadata &local, const SpotifyTrackInfo &spotify)
{
    if (!sameTextStrict(local.title, spotify.title))
        return false;
    if (!local.album.empty() && !sameAlbumTextStrict(local.album, spotify.album))
        return false;
    if (local.lengthSeconds > 0.0 && spotify.durationMs > 0)
    {
        const double spotifySeconds = static_cast<double>(spotify.durationMs) / 1000.0;
        if (std::abs(local.lengthSeconds - spotifySeconds) > 3.0)
            return false;
    }
    if (sameTextStrict(local.artist, spotify.artist))
        return true;
    return !local.album.empty() && sameAlbumTextStrict(local.album, spotify.album);
}

bool isSpotifyVirtualPath(const std::string &path)
{
    return path.rfind("spotify:track:", 0) == 0;
}

bool isLocalFilesystemPath(const std::string &path)
{
    if (path.empty() || isSpotifyVirtualPath(path))
        return false;
    try
    {
        return !filesystem::g_is_remote_or_unrecognized(path.c_str());
    }
    catch (...)
    {
        return false;
    }
}

std::string trimLink(std::string input)
{
    input.erase(std::remove_if(input.begin(), input.end(), [](unsigned char ch) { return ch == '\r' || ch == '\n' || ch == '\t'; }), input.end());
    while (!input.empty() && std::isspace(static_cast<unsigned char>(input.front())))
        input.erase(input.begin());
    while (!input.empty() && std::isspace(static_cast<unsigned char>(input.back())))
        input.pop_back();
    return input;
}

std::string htmlDecodeMinimal(std::string value)
{
    for (const auto &pair : {std::pair{"&amp;", "&"}, {"&#x3D;", "="}, {"&#61;", "="}})
    {
        size_t pos = 0;
        while ((pos = value.find(pair.first, pos)) != std::string::npos)
        {
            value.replace(pos, std::strlen(pair.first), pair.second);
            pos += std::strlen(pair.second);
        }
    }
    return value;
}

std::optional<std::string> findOpenSpotifyUrl(const std::string &html)
{
    const std::string marker = "https://open.spotify.com/";
    const size_t begin = html.find(marker);
    if (begin == std::string::npos)
        return std::nullopt;
    size_t end = begin;
    while (end < html.size() && html[end] != '"' && html[end] != '\'' && html[end] != '<' && !std::isspace(static_cast<unsigned char>(html[end])))
        ++end;
    return htmlDecodeMinimal(html.substr(begin, end - begin));
}

std::optional<SpotifyLink> parseSpotifyLink(std::string input)
{
    input = trimLink(input);
    if (input.rfind("https://spotify.link/", 0) == 0 || input.rfind("http://spotify.link/", 0) == 0 ||
        input.rfind("https://spotify.app.link/", 0) == 0 || input.rfind("http://spotify.app.link/", 0) == 0)
    {
        if (const auto finalUrl = httpFinalUrl(input))
            input = *finalUrl;
        if (input.find("open.spotify.com/") == std::string::npos)
        {
            const std::wstring wideInput = pfc::stringcvt::string_wide_from_utf8(input.c_str()).get_ptr();
            if (const auto response = httpRequest(L"GET", wideInput, {}, ""))
            {
                if (const auto openUrl = findOpenSpotifyUrl(response->body))
                    input = *openUrl;
            }
        }
    }

    const std::pair<const char *, SpotifyLinkKind> prefixes[] = {
        {"spotify:track:", SpotifyLinkKind::track},
        {"spotify:album:", SpotifyLinkKind::album},
        {"spotify:playlist:", SpotifyLinkKind::playlist},
        {"spotify:socialsession:", SpotifyLinkKind::socialSession},
    };
    for (const auto &prefix : prefixes)
    {
        const std::string marker = prefix.first;
        if (input.rfind(marker, 0) == 0 && input.size() > marker.size())
            return SpotifyLink{prefix.second, input};
    }

    const std::pair<const char *, SpotifyLinkKind> urlSegments[] = {
        {"/track/", SpotifyLinkKind::track},
        {"/album/", SpotifyLinkKind::album},
        {"/playlist/", SpotifyLinkKind::playlist},
        {"/socialsession/", SpotifyLinkKind::socialSession},
    };
    for (const auto &segment : urlSegments)
    {
        const size_t typePos = input.find(segment.first);
        if (typePos == std::string::npos)
            continue;
        std::string id = input.substr(typePos + std::strlen(segment.first));
        const size_t query = id.find('?');
        if (query != std::string::npos)
            id.resize(query);
        const size_t slash = id.find('/');
        if (slash != std::string::npos)
            id.resize(slash);
        if (id.empty())
            continue;

        const char *uriPrefix = segment.second == SpotifyLinkKind::track
                                    ? "spotify:track:"
                                    : (segment.second == SpotifyLinkKind::album
                                           ? "spotify:album:"
                                           : (segment.second == SpotifyLinkKind::playlist ? "spotify:playlist:" : "spotify:socialsession:"));
        return SpotifyLink{segment.second, std::string(uriPrefix) + id};
    }
    return std::nullopt;
}

template <typename PlaylistManagerPtr>
std::optional<t_size> findPlaylistByName(PlaylistManagerPtr &playlists, const char *playlistName)
{
    const t_size count = playlists->get_playlist_count();
    for (t_size index = 0; index < count; ++index)
    {
        pfc::string8 name;
        if (playlists->playlist_get_name(index, name) && std::strcmp(name.c_str(), playlistName) == 0)
            return index;
    }
    return std::nullopt;
}

void addLocationsToPlaylist(const std::vector<std::string> &uris, const char *playlistName, bool select, bool reuseExisting)
{
    auto playlists = static_api_ptr_t<playlist_manager>();
    t_size playlist = SIZE_MAX;
    if (reuseExisting)
    {
        if (const auto existing = findPlaylistByName(playlists, playlistName))
            playlist = *existing;
    }
    if (playlist == SIZE_MAX)
        playlist = playlists->create_playlist(playlistName, SIZE_MAX, SIZE_MAX);
    if (playlist == SIZE_MAX)
    {
        popup_message::g_show("新しい playlist を作成できませんでした。", "Spotify Linker");
        return;
    }

    playlists->set_active_playlist(playlist);
    const t_size firstInserted = playlists->playlist_get_item_count(playlist);
    t_size insertAt = firstInserted;
    for (const auto &text : uris)
    {
        const char *uri = text.c_str();
        if (playlists->playlist_insert_locations(playlist, insertAt, pfc::list_single_ref_t<const char *>(uri), select, core_api::get_main_window()))
            ++insertAt;
    }
    if (insertAt > firstInserted)
    {
        playlists->playlist_set_focus_item(playlist, firstInserted);
        playlists->playlist_ensure_visible(playlist, firstInserted);
    }
}

void addLocationsToNewPlaylist(const std::vector<std::string> &uris, const char *playlistName, bool select)
{
    addLocationsToPlaylist(uris, playlistName, select, false);
}

void showLibraryAutoLinkComplete(LibraryAutoLinkStats stats)
{
    pfc::string8 message;
    message << "Library 自動連携が完了しました。\n"
            << "対象: " << stats.total << "\n"
            << "登録: " << stats.registered << "\n"
            << "登録済み: " << stats.alreadyMapped << "\n"
            << "曖昧/不一致でスキップ: " << stats.skipped << "\n"
            << "候補不一致: " << stats.candidatesRejected << "\n"
            << "検索失敗: " << stats.failed;
    popup_message::g_show(message.c_str(), "Spotify Linker");
}

void runLibraryAutoLink(std::vector<TrackMetadata> tracks)
{
    std::thread([tracks = std::move(tracks)]() mutable {
        LibraryAutoLinkStats stats;
        stats.total = tracks.size();

        SpotifyApiClient client;
        for (const auto &metadata : tracks)
        {
            if (metadata.title.empty() || metadata.artist.empty() || !isLocalFilesystemPath(metadata.path))
            {
                ++stats.skipped;
                continue;
            }

            const std::string localHash = makeLocalHash(metadata);
            if (MappingManager::instance().getTrackMapping(localHash))
            {
                ++stats.alreadyMapped;
                continue;
            }

            std::optional<std::string> matchedUri;
            bool hadCandidates = false;
            for (const auto &query : makeTrackSearchQueries(metadata))
            {
                const auto uris = client.searchTracks(query, 5);
                if (!uris.empty())
                    hadCandidates = true;

                for (const auto &uri : uris)
                {
                    const auto spotifyInfo = client.getTrackInfo(uri);
                    if (spotifyInfo && isStrictTrackMatch(metadata, *spotifyInfo))
                    {
                        matchedUri = uri;
                        break;
                    }
                    ++stats.candidatesRejected;
                }
                if (matchedUri)
                    break;
            }
            if (!matchedUri)
            {
                if (hadCandidates)
                {
                    ++stats.skipped;
                    FB2K_console_formatter() << "foo_spotify_linker: Library auto link skipped: "
                                             << metadata.artist.c_str() << " - " << metadata.title.c_str();
                }
                else
                {
                    ++stats.failed;
                    FB2K_console_formatter() << "foo_spotify_linker: Library auto link search failed: "
                                             << metadata.artist.c_str() << " - " << metadata.title.c_str();
                }
                continue;
            }

            if (MappingManager::instance().addTrackMapping(localHash, *matchedUri))
                ++stats.registered;
            else
                ++stats.failed;
        }

        fb2k::inMainThread([stats] { showLibraryAutoLinkComplete(stats); });
    }).detach();
}

void startLibraryAutoLink()
{
    auto library = library_manager::get();
    if (!library->is_library_enabled())
    {
        popup_message::g_show("Media Library が有効ではありません。foobar2000 の Library 設定を確認してください。", "Spotify Linker");
        return;
    }

    metadb_handle_list handles;
    library->get_all_items(handles);
    if (handles.get_count() == 0)
    {
        popup_message::g_show("Media Library に曲がありません。", "Spotify Linker");
        return;
    }

    std::vector<TrackMetadata> tracks;
    tracks.reserve(handles.get_count());
    for (t_size index = 0; index < handles.get_count(); ++index)
        tracks.push_back(readTrackMetadata(handles.get_item(index)));

    popup_message::g_show(("Library の " + std::to_string(tracks.size()) + " 曲をバックグラウンドでSpotify自動連携します。曖昧な候補は登録しません。").c_str(), "Spotify Linker");
    runLibraryAutoLink(std::move(tracks));
}

std::vector<std::string> getCurrentAndNextSpotifyTrackUris()
{
    std::vector<std::string> tracks;
    SpotifyApiClient api;
    if (const auto current = api.getCurrentPlayback())
    {
        if (!current->trackUri.empty())
            tracks.push_back(current->trackUri);
    }
    for (const auto &uri : api.getQueueTrackUris())
    {
        if (std::find(tracks.begin(), tracks.end(), uri) == tracks.end())
        {
            tracks.push_back(uri);
            break;
        }
    }
    return tracks;
}

class AddSpotifyLinkDialog : public CDialogImpl<AddSpotifyLinkDialog>
{
public:
    enum
    {
        IDD = IDD_ADD_SPOTIFY_LINK
    };

    std::string link() const
    {
        return m_link;
    }

    BEGIN_MSG_MAP_EX(AddSpotifyLinkDialog)
    COMMAND_ID_HANDLER_EX(IDOK, onOk)
    COMMAND_ID_HANDLER_EX(IDCANCEL, onCancel)
    END_MSG_MAP()

private:
    void onOk(UINT, int, CWindow)
    {
        CString value;
        GetDlgItemText(IDC_EDIT_SPOTIFY_LINK, value);
        m_link = pfc::stringcvt::string_utf8_from_os(value).get_ptr();
        EndDialog(IDOK);
    }

    void onCancel(UINT, int, CWindow)
    {
        EndDialog(IDCANCEL);
    }

    std::string m_link;
};

class SpotifyMainMenu : public mainmenu_commands
{
public:
    enum Command
    {
        cmd_add_link,
        cmd_autolink_library,
        cmd_count
    };

    t_uint32 get_command_count() override
    {
        return cmd_count;
    }

    GUID get_command(t_uint32 index) override
    {
        switch (index)
        {
        case cmd_add_link:
            return guid_mainmenu_add_link;
        case cmd_autolink_library:
            return guid_mainmenu_autolink_library;
        default:
            uBugCheck();
        }
    }

    void get_name(t_uint32 index, pfc::string_base &out) override
    {
        switch (index)
        {
        case cmd_add_link:
            out = "Add Spotify Link...";
            return;
        case cmd_autolink_library:
            out = "Auto Link Library Tracks";
            return;
        default:
            uBugCheck();
        }
    }

    bool get_description(t_uint32 index, pfc::string_base &out) override
    {
        if (index == cmd_autolink_library)
            out = "Media Library 内の曲を厳格一致したSpotify trackへ自動登録します。";
        else
            out = "Spotify track / album / playlist URL を foobar2000 playlist に追加します。";
        return true;
    }

    GUID get_parent() override
    {
        return guid_mainmenu_group_spotify;
    }

    void execute(t_uint32 index, ctx_t) override
    {
        if (index == cmd_autolink_library)
        {
            startLibraryAutoLink();
            return;
        }

        AddSpotifyLinkDialog dialog;
        if (dialog.DoModal(core_api::get_main_window()) != IDOK)
            return;

        const auto parsed = parseSpotifyLink(dialog.link());
        if (!parsed)
        {
            popup_message::g_show("Spotify track / album / playlist URL または URI を入力してください。", "Spotify Linker");
            return;
        }

        if (parsed->kind == SpotifyLinkKind::playlist)
        {
            const auto tracks = getCurrentAndNextSpotifyTrackUris();
            if (tracks.empty())
            {
                popup_message::g_show("Spotify 側で再生中の曲を取得できませんでした。Spotifyで対象playlistを再生してから、もう一度追加してください。", "Spotify Linker");
                return;
            }
            addLocationsToPlaylist(tracks, "Spotify Playlist", false, true);
            popup_message::g_show(("Spotify Playlist に現在再生と次の曲から " + std::to_string(tracks.size()) + " 曲を追加しました。以後は Follow Spotify playback で1曲ずつ補充します。").c_str(), "Spotify Linker");
            return;
        }

        if (parsed->kind == SpotifyLinkKind::socialSession)
        {
            const auto tracks = getCurrentAndNextSpotifyTrackUris();
            if (tracks.empty())
            {
                popup_message::g_show("Jam の中身は Spotify Web API から直接取得できません。Jam に参加して再生中にしてから、もう一度追加してください。", "Spotify Linker");
                return;
            }
            addLocationsToPlaylist(tracks, "Spotify Playlist", false, true);
            popup_message::g_show(("Spotify Playlist に現在再生と次の曲から " + std::to_string(tracks.size()) + " 曲を追加しました。以後は Follow Spotify playback で1曲ずつ補充します。").c_str(), "Spotify Linker");
            return;
        }

        addLocationsToNewPlaylist({parsed->uri}, parsed->kind == SpotifyLinkKind::album ? "Spotify Album" : "Spotify Track", true);
    }
};

static mainmenu_group_popup_factory g_mainMenuGroup(guid_mainmenu_group_spotify, mainmenu_groups::file_add, mainmenu_commands::sort_priority_base + 20, "Spotify Linker");
static mainmenu_commands_factory_t<SpotifyMainMenu> g_mainMenuCommands;
} // namespace
} // namespace fsl
