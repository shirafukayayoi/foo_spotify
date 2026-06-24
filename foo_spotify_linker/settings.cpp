#include "stdafx.h"
#include "settings.h"

namespace fsl
{
static constexpr GUID guid_cfg_db_path = {0xc58a5e4d, 0x637f, 0x4a79, {0x8c, 0x95, 0x4d, 0xda, 0xae, 0x0f, 0xd7, 0x7e}};
static constexpr GUID guid_cfg_client_id = {0xc91b573b, 0x43db, 0x4e10, {0xa3, 0xad, 0xfe, 0xb9, 0x7a, 0x7f, 0x54, 0xba}};
static constexpr GUID guid_cfg_device_id = {0x3e1e9c61, 0x48ac, 0x4749, {0xa9, 0x7f, 0x52, 0x02, 0xc1, 0xbe, 0xdc, 0xb0}};
static constexpr GUID guid_cfg_polling = {0xc5f4cdf7, 0x5028, 0x486e, {0x98, 0x8b, 0x06, 0x75, 0x86, 0x36, 0x38, 0x00}};
static constexpr GUID guid_cfg_mute = {0x660e5f80, 0x5bc1, 0x498f, {0xb5, 0xd4, 0x19, 0x21, 0xf4, 0x92, 0x23, 0x2e}};
static constexpr GUID guid_cfg_access_token = {0x9a6ddb8e, 0xd728, 0x4fd7, {0xb9, 0xb7, 0x39, 0x13, 0x46, 0x65, 0xa1, 0xb5}};
static constexpr GUID guid_cfg_refresh_token = {0xe6e53f80, 0x0809, 0x4b32, {0xa7, 0x11, 0x93, 0x44, 0x80, 0xea, 0x96, 0x32}};
static constexpr GUID guid_cfg_token_expires_at = {0x542e97d6, 0xc033, 0x4bd7, {0x91, 0x70, 0x73, 0x54, 0xa9, 0x9b, 0xc1, 0x1d}};

cfg_var_modern::cfg_string g_cfg_db_path(guid_cfg_db_path, "");
cfg_var_modern::cfg_string g_cfg_client_id(guid_cfg_client_id, "");
cfg_var_modern::cfg_string g_cfg_default_device_id(guid_cfg_device_id, "");
cfg_var_modern::cfg_int g_cfg_polling_interval_ms(guid_cfg_polling, 1000);
cfg_var_modern::cfg_bool g_cfg_mute_on_sync(guid_cfg_mute, true);
cfg_var_modern::cfg_string g_cfg_access_token(guid_cfg_access_token, "");
cfg_var_modern::cfg_string g_cfg_refresh_token(guid_cfg_refresh_token, "");
cfg_var_modern::cfg_int g_cfg_token_expires_at(guid_cfg_token_expires_at, 0);

std::string effectiveDbPath()
{
    pfc::string8 configured = g_cfg_db_path.get();
    if (!configured.is_empty())
        return configured.c_str();

    pfc::string8 profile;
    filesystem::g_get_native_path(core_api::get_profile_path(), profile);
    return std::string(profile.c_str()) + "\\foo_spotify_linker.db";
}
} // namespace fsl
