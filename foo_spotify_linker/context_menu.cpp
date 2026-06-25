#include "stdafx.h"
#include "mapping_manager.h"
#include "metadata.h"
#include "resource.h"
#include "spotify_api_client.h"

namespace fsl
{
namespace
{
static constexpr GUID guid_context_group_spotify = {0xa1fbfe4d, 0x2a5b, 0x4d84, {0x9f, 0xaa, 0x8b, 0x56, 0x21, 0xe9, 0x22, 0x77}};
static constexpr GUID guid_context_set_uri = {0x4b8ad6bc, 0xd71d, 0x4115, {0x90, 0xa7, 0x09, 0x51, 0x7c, 0xdd, 0x43, 0x08}};
static constexpr GUID guid_context_remove_uri = {0x7898af35, 0xa9ca, 0x4675, {0x89, 0x95, 0x56, 0xd5, 0x2a, 0x58, 0x75, 0xdf}};
static constexpr GUID guid_context_set_album_uri = {0x6a0814c9, 0xd18b, 0x4d29, {0x8c, 0x7c, 0x64, 0x1f, 0xdb, 0xa3, 0x37, 0x41}};
static constexpr GUID guid_context_remove_album_uri = {0x51edbc36, 0x594f, 0x43c8, {0xb9, 0xc0, 0x86, 0x7d, 0x11, 0x53, 0x0d, 0x98}};
static constexpr GUID guid_context_auto_track_uri = {0x61dbe2b0, 0x224f, 0x44b2, {0xa1, 0x17, 0x87, 0x4a, 0x49, 0x5d, 0x49, 0xe3}};
static constexpr GUID guid_context_auto_album_uri = {0x7b442be6, 0xef43, 0x43ab, {0x9d, 0x3a, 0x5d, 0x16, 0xb4, 0x32, 0x98, 0x40}};
static constexpr GUID guid_context_show_track_uri = {0x652a626d, 0x847f, 0x4c64, {0x92, 0x0f, 0xa4, 0x78, 0xd4, 0xd6, 0x9a, 0x56}};
static constexpr GUID guid_context_show_album_uri = {0xb6985adc, 0xc2b4, 0x4a87, {0xaf, 0x0b, 0x2f, 0xd0, 0xf9, 0x89, 0xe5, 0x2a}};

enum class SpotifyUriKind
{
    track,
    album
};

bool normalizeSpotifyUri(std::string input, SpotifyUriKind kind, std::string &out)
{
    input.erase(std::remove_if(input.begin(), input.end(), [](unsigned char ch) { return ch == '\r' || ch == '\n' || ch == '\t'; }), input.end());
    while (!input.empty() && std::isspace(static_cast<unsigned char>(input.front())))
        input.erase(input.begin());
    while (!input.empty() && std::isspace(static_cast<unsigned char>(input.back())))
        input.pop_back();

    const std::string uriPrefix = kind == SpotifyUriKind::track ? "spotify:track:" : "spotify:album:";
    if (input.rfind(uriPrefix, 0) == 0 && input.size() > uriPrefix.size())
    {
        out = input;
        return true;
    }

    const std::string typeSegment = kind == SpotifyUriKind::track ? "/track/" : "/album/";
    const std::string host = "https://open.spotify.com/";
    if (input.rfind(host, 0) == 0)
    {
        const size_t typePos = input.find(typeSegment, host.size());
        if (typePos == std::string::npos)
            return false;
        std::string id = input.substr(typePos + typeSegment.size());
        const size_t query = id.find('?');
        if (query != std::string::npos)
            id.resize(query);
        const size_t slash = id.find('/');
        if (slash != std::string::npos)
            id.resize(slash);
        if (!id.empty())
        {
            out = uriPrefix + id;
            return true;
        }
    }

    return false;
}

bool normalizeSpotifyUriAny(std::string input, std::string &out, SpotifyUriKind &kind)
{
    if (normalizeSpotifyUri(input, SpotifyUriKind::track, out))
    {
        kind = SpotifyUriKind::track;
        return true;
    }
    if (normalizeSpotifyUri(input, SpotifyUriKind::album, out))
    {
        kind = SpotifyUriKind::album;
        return true;
    }
    return false;
}

bool isLocalFilesystemPath(const std::string &path)
{
    if (path.empty() || path.rfind("spotify:", 0) == 0)
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

void addAlbumMate(std::vector<TrackMetadata> &out, std::set<std::string> &seenHashes, const TrackMetadata &seed, const TrackMetadata &candidate)
{
    if (makeAlbumId(candidate) != makeAlbumId(seed))
        return;
    if (!isLocalFilesystemPath(candidate.path))
        return;
    const std::string hash = makeLocalHash(candidate);
    if (seenHashes.insert(hash).second)
        out.push_back(candidate);
}

std::vector<TrackMetadata> collectAlbumMates(const TrackMetadata &seed, metadb_handle_list_cref selected)
{
    std::vector<TrackMetadata> tracks;
    std::set<std::string> seenHashes;

    for (t_size index = 0; index < selected.get_count(); ++index)
        addAlbumMate(tracks, seenHashes, seed, readTrackMetadata(selected.get_item(index)));

    auto library = library_manager::get();
    if (library->is_library_enabled())
    {
        metadb_handle_list handles;
        library->get_all_items(handles);
        for (t_size index = 0; index < handles.get_count(); ++index)
            addAlbumMate(tracks, seenHashes, seed, readTrackMetadata(handles.get_item(index)));
    }

    auto playlists = static_api_ptr_t<playlist_manager>();
    const t_size activePlaylist = playlists->get_active_playlist();
    if (activePlaylist != SIZE_MAX)
    {
        const t_size count = playlists->playlist_get_item_count(activePlaylist);
        for (t_size index = 0; index < count; ++index)
        {
            metadb_handle_ptr handle;
            if (playlists->playlist_get_item_handle(handle, activePlaylist, index))
                addAlbumMate(tracks, seenHashes, seed, readTrackMetadata(handle));
        }
    }

    return tracks;
}

size_t registerAlbumTrackMappings(const TrackMetadata &seed, metadb_handle_list_cref selected, const std::string &spotifyAlbumUri)
{
    const auto spotifyTracks = SpotifyApiClient().getAlbumTrackUris(spotifyAlbumUri);
    if (spotifyTracks.empty())
        return 0;

    size_t registered = 0;
    for (const auto &metadata : collectAlbumMates(seed, selected))
    {
        if (metadata.trackNumber <= 0)
            continue;
        const size_t index = static_cast<size_t>(metadata.trackNumber - 1);
        if (index >= spotifyTracks.size())
            continue;
        if (MappingManager::instance().addTrackMapping(makeLocalHash(metadata), spotifyTracks[index]))
            ++registered;
    }
    return registered;
}

size_t registerAlbumTrackMappingsFromTrackUri(const TrackMetadata &seed, metadb_handle_list_cref selected, const std::string &spotifyTrackUri)
{
    const auto info = SpotifyApiClient().getTrackInfo(spotifyTrackUri);
    if (!info || info->albumUri.empty())
        return 0;
    return registerAlbumTrackMappings(seed, selected, info->albumUri);
}

class UriDialog : public CDialogImpl<UriDialog>
{
public:
    enum
    {
        IDD = IDD_URI_DIALOG
    };

    UriDialog(const TrackMetadata &metadata, SpotifyUriKind kind) : m_metadata(metadata), m_kind(kind) {}

    std::string uri() const
    {
        return m_uri;
    }

    SpotifyUriKind resolvedKind() const
    {
        return m_resolvedKind;
    }

    BEGIN_MSG_MAP_EX(UriDialog)
    MSG_WM_INITDIALOG(onInitDialog)
    COMMAND_ID_HANDLER_EX(IDOK, onOk)
    COMMAND_ID_HANDLER_EX(IDCANCEL, onCancel)
    END_MSG_MAP()

private:
    BOOL onInitDialog(CWindow, LPARAM)
    {
        std::string label;
        if (m_kind == SpotifyUriKind::album)
            label = "Album: " + (m_metadata.albumArtist.empty() ? m_metadata.artist : m_metadata.albumArtist) + " - " + m_metadata.album;
        else
            label = m_metadata.artist + " - " + m_metadata.title;
        if (label == " - ")
            label = m_metadata.path;
        SetDlgItemText(IDC_STATIC_TRACK, pfc::stringcvt::string_os_from_utf8(label.c_str()));
        return TRUE;
    }

    void onOk(UINT, int, CWindow)
    {
        CString value;
        GetDlgItemText(IDC_EDIT_SPOTIFY_URI, value);
        std::string normalized;
        SpotifyUriKind resolvedKind = m_kind;
        const bool ok = m_kind == SpotifyUriKind::track
                            ? normalizeSpotifyUriAny(pfc::stringcvt::string_utf8_from_os(value).get_ptr(), normalized, resolvedKind)
                            : normalizeSpotifyUri(pfc::stringcvt::string_utf8_from_os(value).get_ptr(), m_kind, normalized);
        if (!ok)
        {
            popup_message::g_show("Spotify URI または open.spotify.com の URL を入力してください。", "Spotify Linker");
            return;
        }
        m_uri = normalized;
        m_resolvedKind = resolvedKind;
        EndDialog(IDOK);
    }

    void onCancel(UINT, int, CWindow)
    {
        EndDialog(IDCANCEL);
    }

    TrackMetadata m_metadata;
    SpotifyUriKind m_kind;
    SpotifyUriKind m_resolvedKind = SpotifyUriKind::track;
    std::string m_uri;
};

class SpotifyContextMenu : public contextmenu_item_simple
{
public:
    enum
    {
        cmd_set_uri = 0,
        cmd_auto_track_uri,
        cmd_show_track_uri,
        cmd_remove_uri,
        cmd_set_album_uri,
        cmd_auto_album_uri,
        cmd_count
    };

    unsigned get_num_items() override
    {
        return cmd_count;
    }

    void get_item_name(unsigned index, pfc::string_base &out) override
    {
        switch (index)
        {
        case cmd_set_uri:
            out = "Set Track URI...";
            return;
        case cmd_auto_track_uri:
            out = "Auto Set Track URI";
            return;
        case cmd_show_track_uri:
            out = "Show Track URI";
            return;
        case cmd_remove_uri:
            out = "Remove Track URI";
            return;
        case cmd_set_album_uri:
            out = "Set Track URI from Album URL...";
            return;
        case cmd_auto_album_uri:
            out = "Auto Set Track URI from Album";
            return;
        default:
            uBugCheck();
        }
    }

    bool get_item_description(unsigned index, pfc::string_base &out) override
    {
        switch (index)
        {
        case cmd_set_uri:
            out = "選択トラックへ Spotify URI を手動マッピングします。";
            return true;
        case cmd_remove_uri:
            out = "選択トラックの Spotify URI マッピングを削除します。";
            return true;
        case cmd_auto_track_uri:
            out = "選択トラックのタグから Spotify track を検索して登録します。";
            return true;
        case cmd_show_track_uri:
            out = "選択トラックに登録済みの Spotify track URI を表示します。";
            return true;
        case cmd_set_album_uri:
            out = "Spotify album URL から選択トラックに対応する Spotify track URI を取得して登録します。";
            return true;
        case cmd_auto_album_uri:
            out = "選択トラックのアルバムタグから Spotify album を検索し、対応する Spotify track URI を登録します。";
            return true;
        default:
            return false;
        }
    }

    GUID get_item_guid(unsigned index) override
    {
        switch (index)
        {
        case cmd_set_uri:
            return guid_context_set_uri;
        case cmd_remove_uri:
            return guid_context_remove_uri;
        case cmd_auto_track_uri:
            return guid_context_auto_track_uri;
        case cmd_show_track_uri:
            return guid_context_show_track_uri;
        case cmd_set_album_uri:
            return guid_context_set_album_uri;
        case cmd_auto_album_uri:
            return guid_context_auto_album_uri;
        default:
            uBugCheck();
        }
    }

    GUID get_parent() override
    {
        return guid_context_group_spotify;
    }

    void context_command(unsigned index, metadb_handle_list_cref data, const GUID &) override
    {
        if (data.get_count() == 0)
            return;

        const TrackMetadata metadata = readTrackMetadata(data.get_item(0));
        const std::string localHash = makeLocalHash(metadata);
        if (index == cmd_show_track_uri)
        {
            const auto uri = MappingManager::instance().getTrackMapping(localHash);
            popup_message::g_show((uri ? "Track URI:\n" + *uri : "Track URI は未登録です。").c_str(), "Spotify Linker");
            return;
        }
        if (index == cmd_remove_uri)
        {
            MappingManager::instance().removeTrackMapping(localHash);
            return;
        }
        if (index == cmd_auto_track_uri)
        {
            SpotifyApiClient client;
            std::optional<std::string> uri;
            for (const auto &query : makeTrackSearchQueries(metadata))
            {
                uri = client.searchTrack(query);
                if (uri)
                    break;
            }
            if (!uri)
            {
                popup_message::g_show("Spotify track を検索できませんでした。", "Spotify Linker");
                return;
            }
            MappingManager::instance().addTrackMapping(localHash, *uri);
            popup_message::g_show(("Track URI を登録しました:\n" + *uri).c_str(), "Spotify Linker");
            return;
        }
        if (index == cmd_auto_album_uri)
        {
            SpotifyApiClient client;
            std::optional<std::string> albumUri;
            for (const auto &query : makeAlbumSearchQueries(metadata))
            {
                albumUri = client.searchAlbum(query);
                if (albumUri)
                    break;
            }
            if (!albumUri)
            {
                popup_message::g_show("Spotify album を検索できませんでした。", "Spotify Linker");
                return;
            }
            const size_t registered = registerAlbumTrackMappings(metadata, data, *albumUri);
            if (registered == 0)
            {
                popup_message::g_show("Spotify album から登録できる track URI を取得できませんでした。", "Spotify Linker");
                return;
            }
            popup_message::g_show(("Album 内の " + std::to_string(registered) + " 曲へ Track URI を登録しました。").c_str(), "Spotify Linker");
            return;
        }

        const bool albumMode = index == cmd_set_album_uri;
        UriDialog dialog(metadata, albumMode ? SpotifyUriKind::album : SpotifyUriKind::track);
        if (dialog.DoModal(core_api::get_main_window()) == IDOK)
        {
            if (albumMode)
            {
                const size_t registered = registerAlbumTrackMappings(metadata, data, dialog.uri());
                if (registered == 0)
                {
                    popup_message::g_show("Album URL から登録できる track URI を取得できませんでした。", "Spotify Linker");
                    return;
                }
                popup_message::g_show(("Album 内の " + std::to_string(registered) + " 曲へ Track URI を登録しました。").c_str(), "Spotify Linker");
            }
            else
            {
                std::string trackUri = dialog.uri();
                if (dialog.resolvedKind() == SpotifyUriKind::album)
                {
                    const size_t registered = registerAlbumTrackMappings(metadata, data, dialog.uri());
                    if (registered == 0)
                    {
                        popup_message::g_show("Album URL から登録できる track URI を取得できませんでした。", "Spotify Linker");
                        return;
                    }
                    popup_message::g_show(("Album 内の " + std::to_string(registered) + " 曲へ Track URI を登録しました。").c_str(), "Spotify Linker");
                    return;
                }
                const size_t registered = registerAlbumTrackMappingsFromTrackUri(metadata, data, trackUri);
                if (registered > 0)
                {
                    popup_message::g_show(("Track URL の album から " + std::to_string(registered) + " 曲へ Track URI を登録しました。").c_str(), "Spotify Linker");
                    return;
                }
                MappingManager::instance().addTrackMapping(localHash, trackUri);
                popup_message::g_show(("Track URI を登録しました:\n" + trackUri).c_str(), "Spotify Linker");
            }
        }
    }

    bool context_get_display(unsigned index, metadb_handle_list_cref data, pfc::string_base &out, unsigned &displayFlags, const GUID &) override
    {
        displayFlags = 0;
        get_item_name(index, out);
        return data.get_count() > 0;
    }
};

contextmenu_group_popup_factory g_contextGroup(guid_context_group_spotify, contextmenu_groups::root, "Spotify Linker", -10);
contextmenu_item_factory_t<SpotifyContextMenu> g_contextMenu;
} // namespace
} // namespace fsl
