#pragma once

namespace fsl
{
extern cfg_var_modern::cfg_string g_cfg_db_path;
extern cfg_var_modern::cfg_string g_cfg_client_id;
extern cfg_var_modern::cfg_string g_cfg_default_device_id;
extern cfg_var_modern::cfg_int g_cfg_polling_interval_ms;
extern cfg_var_modern::cfg_bool g_cfg_mute_on_sync;
extern cfg_var_modern::cfg_string g_cfg_access_token;
extern cfg_var_modern::cfg_string g_cfg_refresh_token;
extern cfg_var_modern::cfg_int g_cfg_token_expires_at;

std::string effectiveDbPath();
constexpr const char *kRedirectUri = "http://127.0.0.1:8088/callback";
} // namespace fsl
