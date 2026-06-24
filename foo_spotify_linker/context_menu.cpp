#include "stdafx.h"
#include "mapping_manager.h"
#include "metadata.h"
#include "resource.h"

namespace fsl
{
namespace
{
static constexpr GUID guid_context_set_uri = {0x4b8ad6bc, 0xd71d, 0x4115, {0x90, 0xa7, 0x09, 0x51, 0x7c, 0xdd, 0x43, 0x08}};
static constexpr GUID guid_context_remove_uri = {0x7898af35, 0xa9ca, 0x4675, {0x89, 0x95, 0x56, 0xd5, 0x2a, 0x58, 0x75, 0xdf}};

bool normalizeSpotifyUri(std::string input, std::string &out)
{
    input.erase(std::remove_if(input.begin(), input.end(), [](unsigned char ch) { return ch == '\r' || ch == '\n' || ch == '\t'; }), input.end());
    while (!input.empty() && std::isspace(static_cast<unsigned char>(input.front())))
        input.erase(input.begin());
    while (!input.empty() && std::isspace(static_cast<unsigned char>(input.back())))
        input.pop_back();

    const std::string uriPrefix = "spotify:track:";
    if (input.rfind(uriPrefix, 0) == 0 && input.size() > uriPrefix.size())
    {
        out = input;
        return true;
    }

    const std::string urlPrefix = "https://open.spotify.com/track/";
    if (input.rfind(urlPrefix, 0) == 0)
    {
        std::string id = input.substr(urlPrefix.size());
        const size_t query = id.find('?');
        if (query != std::string::npos)
            id.resize(query);
        if (!id.empty())
        {
            out = uriPrefix + id;
            return true;
        }
    }

    return false;
}

class UriDialog : public CDialogImpl<UriDialog>
{
public:
    enum
    {
        IDD = IDD_URI_DIALOG
    };

    explicit UriDialog(const TrackMetadata &metadata) : m_metadata(metadata) {}

    std::string uri() const
    {
        return m_uri;
    }

    BEGIN_MSG_MAP_EX(UriDialog)
    MSG_WM_INITDIALOG(onInitDialog)
    COMMAND_ID_HANDLER_EX(IDOK, onOk)
    COMMAND_ID_HANDLER_EX(IDCANCEL, onCancel)
    END_MSG_MAP()

private:
    BOOL onInitDialog(CWindow, LPARAM)
    {
        std::string label = m_metadata.artist + " - " + m_metadata.title;
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
        if (!normalizeSpotifyUri(pfc::stringcvt::string_utf8_from_os(value).get_ptr(), normalized))
        {
            popup_message::g_show("Spotify URI または open.spotify.com の track URL を入力してください。", "Spotify Linker");
            return;
        }
        m_uri = normalized;
        EndDialog(IDOK);
    }

    void onCancel(UINT, int, CWindow)
    {
        EndDialog(IDCANCEL);
    }

    TrackMetadata m_metadata;
    std::string m_uri;
};

class SpotifyContextMenu : public contextmenu_item_simple
{
public:
    enum
    {
        cmd_set_uri = 0,
        cmd_remove_uri,
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
            out = "Set Spotify URI...";
            return;
        case cmd_remove_uri:
            out = "Remove Spotify URI";
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
        default:
            uBugCheck();
        }
    }

    GUID get_parent() override
    {
        return contextmenu_groups::root;
    }

    void context_command(unsigned index, metadb_handle_list_cref data, const GUID &) override
    {
        if (data.get_count() == 0)
            return;

        const TrackMetadata metadata = readTrackMetadata(data.get_item(0));
        const std::string localHash = makeLocalHash(metadata);
        if (index == cmd_remove_uri)
        {
            MappingManager::instance().removeTrackMapping(localHash);
            return;
        }

        UriDialog dialog(metadata);
        if (dialog.DoModal(core_api::get_main_window()) == IDOK)
        {
            MappingManager::instance().addTrackMapping(localHash, dialog.uri());
        }
    }

    bool context_get_display(unsigned index, metadb_handle_list_cref data, pfc::string_base &out, unsigned &displayFlags, const GUID &) override
    {
        displayFlags = 0;
        get_item_name(index, out);
        return data.get_count() > 0;
    }
};

contextmenu_item_factory_t<SpotifyContextMenu> g_contextMenu;
} // namespace
} // namespace fsl
