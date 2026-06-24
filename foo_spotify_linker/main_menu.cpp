#include "stdafx.h"
#include "http_client.h"
#include "resource.h"
#include "spotify_api_client.h"

namespace fsl
{
namespace
{
static constexpr GUID guid_mainmenu_group_spotify = {0xf7c54c3f, 0x8bc2, 0x4ca2, {0x91, 0xac, 0x0e, 0x8f, 0xd6, 0xf5, 0x46, 0x42}};
static constexpr GUID guid_mainmenu_add_link = {0xc2f2cf2a, 0x33fb, 0x43b5, {0x94, 0xcb, 0x25, 0xcd, 0x64, 0x8a, 0x0e, 0x55}};

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

void addLocationsToNewPlaylist(const std::vector<std::string> &uris, const char *playlistName, bool select)
{
    auto playlists = static_api_ptr_t<playlist_manager>();
    const t_size playlist = playlists->create_playlist(playlistName, SIZE_MAX, SIZE_MAX);
    if (playlist == SIZE_MAX)
    {
        popup_message::g_show("新しい playlist を作成できませんでした。", "Spotify Linker");
        return;
    }

    playlists->set_active_playlist(playlist);
    t_size insertAt = 0;
    for (const auto &text : uris)
    {
        const char *uri = text.c_str();
        if (playlists->playlist_insert_locations(playlist, insertAt, pfc::list_single_ref_t<const char *>(uri), select, core_api::get_main_window()))
            ++insertAt;
    }
    if (insertAt > 0)
    {
        playlists->playlist_set_focus_item(playlist, 0);
        playlists->playlist_ensure_visible(playlist, 0);
    }
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
    t_uint32 get_command_count() override
    {
        return 1;
    }

    GUID get_command(t_uint32) override
    {
        return guid_mainmenu_add_link;
    }

    void get_name(t_uint32, pfc::string_base &out) override
    {
        out = "Add Spotify Link...";
    }

    bool get_description(t_uint32, pfc::string_base &out) override
    {
        out = "Spotify track / album / playlist URL を foobar2000 playlist に追加します。";
        return true;
    }

    GUID get_parent() override
    {
        return guid_mainmenu_group_spotify;
    }

    void execute(t_uint32, ctx_t) override
    {
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
            const auto tracks = SpotifyApiClient().getPlaylistTrackUris(parsed->uri);
            if (tracks.empty())
            {
                popup_message::g_show("Spotify playlist の曲を取得できませんでした。再ログインが必要な場合があります。", "Spotify Linker");
                return;
            }
            addLocationsToNewPlaylist(tracks, "Spotify Playlist", false);
            popup_message::g_show(("Playlist から " + std::to_string(tracks.size()) + " 曲を追加しました。").c_str(), "Spotify Linker");
            return;
        }

        if (parsed->kind == SpotifyLinkKind::socialSession)
        {
            std::vector<std::string> tracks;
            SpotifyApiClient api;
            if (const auto current = api.getCurrentPlayback())
                tracks.push_back(current->trackUri);
            for (const auto &uri : api.getQueueTrackUris())
            {
                if (std::find(tracks.begin(), tracks.end(), uri) == tracks.end())
                    tracks.push_back(uri);
            }
            if (tracks.empty())
            {
                popup_message::g_show("Jam の中身は Spotify Web API から直接取得できません。Jam に参加して再生中にしてから、もう一度追加してください。", "Spotify Linker");
                return;
            }
            addLocationsToNewPlaylist(tracks, "Spotify Jam", false);
            popup_message::g_show(("Jam の現在再生と queue から " + std::to_string(tracks.size()) + " 曲を追加しました。新しく追加される曲は Follow Spotify playback を有効にすると追従します。").c_str(), "Spotify Linker");
            return;
        }

        addLocationsToNewPlaylist({parsed->uri}, parsed->kind == SpotifyLinkKind::album ? "Spotify Album" : "Spotify Track", true);
    }
};

static mainmenu_group_popup_factory g_mainMenuGroup(guid_mainmenu_group_spotify, mainmenu_groups::file_add, mainmenu_commands::sort_priority_base + 20, "Spotify Linker");
static mainmenu_commands_factory_t<SpotifyMainMenu> g_mainMenuCommands;
} // namespace
} // namespace fsl
