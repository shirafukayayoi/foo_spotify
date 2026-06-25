#include "stdafx.h"
#include "auth_manager.h"
#include "resource.h"
#include "settings.h"
#include "spotify_api_client.h"

namespace fsl
{
namespace
{
static constexpr GUID guid_preferences_page = {0x305090bb, 0xfb2a, 0x4034, {0xaf, 0x78, 0x24, 0x5d, 0x67, 0xaa, 0xcc, 0x5c}};

class PreferencesDialog : public CDialogImpl<PreferencesDialog>, public preferences_page_instance
{
public:
    enum
    {
        IDD = IDD_PREFERENCES
    };

    explicit PreferencesDialog(preferences_page_callback::ptr callback) : m_callback(callback) {}

    t_uint32 get_state() override
    {
        t_uint32 state = preferences_state::resettable;
        if (hasChanged())
            state |= preferences_state::changed;
        return state;
    }

    fb2k::hwnd_t get_wnd() override
    {
        return m_hWnd;
    }

    void apply() override
    {
        CString dbPath;
        CString clientId;
        CString deviceId;
        CString polling;
        GetDlgItemText(IDC_EDIT_DB_PATH, dbPath);
        GetDlgItemText(IDC_EDIT_CLIENT_ID, clientId);
        GetDlgItemText(IDC_EDIT_DEVICE_ID, deviceId);
        GetDlgItemText(IDC_EDIT_POLLING_INTERVAL, polling);

        g_cfg_db_path = pfc::stringcvt::string_utf8_from_os(dbPath);
        g_cfg_client_id = pfc::stringcvt::string_utf8_from_os(clientId);
        g_cfg_default_device_id = pfc::stringcvt::string_utf8_from_os(deviceId);
        const int requestedPolling = _wtoi(polling);
        g_cfg_polling_interval_ms = requestedPolling < 250 ? 250 : requestedPolling;
        g_cfg_mute_on_sync = (IsDlgButtonChecked(IDC_CHECK_MUTE_ON_SYNC) == BST_CHECKED);
        g_cfg_follow_spotify_playback = (IsDlgButtonChecked(IDC_CHECK_FOLLOW_SPOTIFY) == BST_CHECKED);
        onChanged();
    }

    void reset() override
    {
        SetDlgItemText(IDC_EDIT_DB_PATH, L"");
        SetDlgItemText(IDC_EDIT_CLIENT_ID, L"");
        SetDlgItemText(IDC_EDIT_DEVICE_ID, L"");
        SetDlgItemText(IDC_EDIT_POLLING_INTERVAL, L"1000");
        CheckDlgButton(IDC_CHECK_MUTE_ON_SYNC, BST_CHECKED);
        CheckDlgButton(IDC_CHECK_FOLLOW_SPOTIFY, BST_UNCHECKED);
        onChanged();
    }

    BEGIN_MSG_MAP_EX(PreferencesDialog)
    MSG_WM_INITDIALOG(onInitDialog)
    COMMAND_HANDLER_EX(IDC_EDIT_DB_PATH, EN_CHANGE, onAnyChange)
    COMMAND_HANDLER_EX(IDC_EDIT_CLIENT_ID, EN_CHANGE, onAnyChange)
    COMMAND_HANDLER_EX(IDC_EDIT_DEVICE_ID, EN_CHANGE, onAnyChange)
    COMMAND_HANDLER_EX(IDC_EDIT_POLLING_INTERVAL, EN_CHANGE, onAnyChange)
    COMMAND_HANDLER_EX(IDC_CHECK_MUTE_ON_SYNC, BN_CLICKED, onAnyChange)
    COMMAND_HANDLER_EX(IDC_CHECK_FOLLOW_SPOTIFY, BN_CLICKED, onAnyChange)
    COMMAND_HANDLER_EX(IDC_BTN_BROWSE_DB, BN_CLICKED, onBrowseDb)
    COMMAND_HANDLER_EX(IDC_BTN_LOGIN, BN_CLICKED, onLogin)
    COMMAND_HANDLER_EX(IDC_BTN_CLEAR_TOKEN, BN_CLICKED, onClearToken)
    COMMAND_HANDLER_EX(IDC_BTN_USE_CURRENT_DEVICE, BN_CLICKED, onUseCurrentDevice)
    END_MSG_MAP()

private:
    BOOL onInitDialog(CWindow, LPARAM)
    {
        SetDlgItemText(IDC_EDIT_DB_PATH, pfc::stringcvt::string_os_from_utf8(g_cfg_db_path.get()));
        SetDlgItemText(IDC_EDIT_CLIENT_ID, pfc::stringcvt::string_os_from_utf8(g_cfg_client_id.get()));
        SetDlgItemText(IDC_EDIT_DEVICE_ID, pfc::stringcvt::string_os_from_utf8(g_cfg_default_device_id.get()));

        wchar_t buf[32] = {};
        _snwprintf_s(buf, _TRUNCATE, L"%d", static_cast<int>(g_cfg_polling_interval_ms.get()));
        SetDlgItemText(IDC_EDIT_POLLING_INTERVAL, buf);
        CheckDlgButton(IDC_CHECK_MUTE_ON_SYNC, g_cfg_mute_on_sync.get() ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(IDC_CHECK_FOLLOW_SPOTIFY, g_cfg_follow_spotify_playback.get() ? BST_CHECKED : BST_UNCHECKED);

        SetDlgItemText(IDC_STATIC_STATUS, statusText().c_str());
        return FALSE;
    }

    void onAnyChange(UINT, int, CWindow)
    {
        onChanged();
    }

    void onBrowseDb(UINT, int, CWindow)
    {
        OPENFILENAME ofn{};
        wchar_t buf[MAX_PATH] = {};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = m_hWnd;
        ofn.lpstrFilter = L"SQLite DB\0*.db\0All Files\0*.*\0";
        ofn.lpstrFile = buf;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST;
        if (GetSaveFileName(&ofn))
            SetDlgItemText(IDC_EDIT_DB_PATH, buf);
        onChanged();
    }

    void onLogin(UINT, int, CWindow)
    {
        apply();
        SetDlgItemText(IDC_STATIC_STATUS, L"ブラウザで Spotify login を完了してください...");
        std::string message;
        const bool ok = AuthManager::instance().beginLogin(message);
        SetDlgItemText(IDC_STATIC_STATUS, pfc::stringcvt::string_os_from_utf8(message.c_str()));
        if (!ok)
            popup_message::g_show(message.c_str(), "Spotify Linker");
        onChanged();
    }

    void onClearToken(UINT, int, CWindow)
    {
        AuthManager::instance().clearTokens();
        SetDlgItemText(IDC_STATIC_STATUS, statusText().c_str());
        onChanged();
    }

    void onUseCurrentDevice(UINT, int, CWindow)
    {
        apply();
        const auto playback = SpotifyApiClient().getCurrentPlayback();
        if (!playback || playback->deviceId.empty())
        {
            popup_message::g_show("現在再生中のSpotifyデバイスIDを取得できませんでした。Spotifyで何か再生してからもう一度押してください。", "Spotify Linker");
            SetDlgItemText(IDC_STATIC_STATUS, L"現在再生中のSpotifyデバイスIDを取得できませんでした。");
            return;
        }

        SetDlgItemText(IDC_EDIT_DEVICE_ID, pfc::stringcvt::string_os_from_utf8(playback->deviceId.c_str()));
        g_cfg_default_device_id = playback->deviceId.c_str();
        std::string message = "現在再生中デバイスを設定しました: ";
        if (!playback->deviceName.empty())
            message += playback->deviceName;
        else
            message += playback->deviceId;
        if (!playback->deviceType.empty())
            message += " (" + playback->deviceType + ")";
        SetDlgItemText(IDC_STATIC_STATUS, pfc::stringcvt::string_os_from_utf8(message.c_str()));
        onChanged();
    }

    bool hasChanged()
    {
        if (!m_hWnd)
            return false;

        CString dbPath;
        CString clientId;
        CString deviceId;
        CString polling;
        GetDlgItemText(IDC_EDIT_DB_PATH, dbPath);
        GetDlgItemText(IDC_EDIT_CLIENT_ID, clientId);
        GetDlgItemText(IDC_EDIT_DEVICE_ID, deviceId);
        GetDlgItemText(IDC_EDIT_POLLING_INTERVAL, polling);

        if (CString(pfc::stringcvt::string_os_from_utf8(g_cfg_db_path.get())) != dbPath)
            return true;
        if (CString(pfc::stringcvt::string_os_from_utf8(g_cfg_client_id.get())) != clientId)
            return true;
        if (CString(pfc::stringcvt::string_os_from_utf8(g_cfg_default_device_id.get())) != deviceId)
            return true;
        if (_wtoi(polling) != static_cast<int>(g_cfg_polling_interval_ms.get()))
            return true;
        if ((IsDlgButtonChecked(IDC_CHECK_MUTE_ON_SYNC) == BST_CHECKED) != g_cfg_mute_on_sync.get())
            return true;
        return (IsDlgButtonChecked(IDC_CHECK_FOLLOW_SPOTIFY) == BST_CHECKED) != g_cfg_follow_spotify_playback.get();
    }

    void onChanged()
    {
        m_callback->on_state_changed();
    }

    std::wstring statusText() const
    {
        pfc::string8 refresh = g_cfg_refresh_token.get();
        if (refresh.is_empty())
            return L"Redirect URI: http://127.0.0.1:8088/callback / 未ログイン";
        return L"Redirect URI: http://127.0.0.1:8088/callback / token 保存済み";
    }

    preferences_page_callback::ptr m_callback;
};

class PreferencesPage : public preferences_page_impl<PreferencesDialog>
{
public:
    const char *get_name() override
    {
        return "Spotify Linker";
    }

    GUID get_guid() override
    {
        return guid_preferences_page;
    }

    GUID get_parent_guid() override
    {
        return preferences_page::guid_tools;
    }
};

preferences_page_factory_t<PreferencesPage> g_preferencesPage;
} // namespace
} // namespace fsl
