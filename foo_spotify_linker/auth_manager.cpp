#include "stdafx.h"
#include "auth_manager.h"
#include "http_client.h"
#include "settings.h"

namespace fsl
{
namespace
{
std::string randomUrlSafe(size_t length)
{
    static constexpr char chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
    std::random_device rd;
    std::mt19937 rng(rd());
    std::uniform_int_distribution<size_t> dist(0, sizeof(chars) - 2);
    std::string out;
    out.reserve(length);
    for (size_t i = 0; i < length; ++i)
        out.push_back(chars[dist(rng)]);
    return out;
}

std::string base64Url(const unsigned char *data, DWORD size)
{
    DWORD needed = 0;
    CryptBinaryToStringA(data, size, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &needed);
    std::string out(needed ? needed - 1 : 0, '\0');
    CryptBinaryToStringA(data, size, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, out.data(), &needed);
    for (char &ch : out)
    {
        if (ch == '+')
            ch = '-';
        else if (ch == '/')
            ch = '_';
    }
    while (!out.empty() && out.back() == '=')
        out.pop_back();
    return out;
}

std::optional<std::string> sha256Base64Url(const std::string &text)
{
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
        return std::nullopt;

    DWORD hashLength = 0;
    DWORD cbData = 0;
    BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashLength), sizeof(hashLength), &cbData, 0);
    BCRYPT_HASH_HANDLE hashHandle = nullptr;
    NTSTATUS status = BCryptCreateHash(alg, &hashHandle, nullptr, 0, nullptr, 0, 0);
    if (status == 0)
        status = BCryptHashData(hashHandle, reinterpret_cast<PUCHAR>(const_cast<char *>(text.data())),
                                static_cast<ULONG>(text.size()), 0);
    std::vector<unsigned char> hash(hashLength);
    if (status == 0)
        status = BCryptFinishHash(hashHandle, hash.data(), hashLength, 0);
    if (hashHandle)
        BCryptDestroyHash(hashHandle);
    BCryptCloseAlgorithmProvider(alg, 0);
    if (status != 0)
        return std::nullopt;
    return base64Url(hash.data(), hashLength);
}

std::optional<std::string> readCallbackCode(const std::string &expectedState)
{
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        return std::nullopt;

    SOCKET server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server == INVALID_SOCKET)
    {
        WSACleanup();
        return std::nullopt;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(8088);
    BOOL reuse = TRUE;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&reuse), sizeof(reuse));
    if (bind(server, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 || listen(server, 1) != 0)
    {
        closesocket(server);
        WSACleanup();
        return std::nullopt;
    }

    fd_set set;
    FD_ZERO(&set);
    FD_SET(server, &set);
    timeval timeout{};
    timeout.tv_sec = 180;
    if (select(0, &set, nullptr, nullptr, &timeout) <= 0)
    {
        closesocket(server);
        WSACleanup();
        return std::nullopt;
    }

    SOCKET client = accept(server, nullptr, nullptr);
    closesocket(server);
    if (client == INVALID_SOCKET)
    {
        WSACleanup();
        return std::nullopt;
    }

    std::array<char, 8192> buffer{};
    const int received = recv(client, buffer.data(), static_cast<int>(buffer.size() - 1), 0);
    std::string request = received > 0 ? std::string(buffer.data(), static_cast<size_t>(received)) : "";

    const char *okBody = "<html><body>Spotify login completed. You can close this window.</body></html>";
    std::string response = std::string("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: ") +
                           std::to_string(std::strlen(okBody)) + "\r\nConnection: close\r\n\r\n" + okBody;
    send(client, response.c_str(), static_cast<int>(response.size()), 0);
    closesocket(client);
    WSACleanup();

    const size_t getPos = request.find("GET ");
    const size_t httpPos = request.find(" HTTP/");
    if (getPos == std::string::npos || httpPos == std::string::npos || httpPos <= getPos + 4)
        return std::nullopt;

    const std::string path = request.substr(getPos + 4, httpPos - (getPos + 4));
    const size_t q = path.find('?');
    if (q == std::string::npos)
        return std::nullopt;
    const std::string query = path.substr(q + 1);

    auto getParam = [&](const std::string &name) -> std::optional<std::string> {
        const std::string needle = name + "=";
        size_t pos = query.find(needle);
        if (pos == std::string::npos)
            return std::nullopt;
        pos += needle.size();
        size_t end = query.find('&', pos);
        return query.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    };

    const auto state = getParam("state");
    if (!state || *state != expectedState)
        return std::nullopt;
    return getParam("code");
}

void saveTokenResponse(const std::string &json)
{
    if (auto access = jsonStringValue(json, "access_token"))
        g_cfg_access_token = access->c_str();
    if (auto refresh = jsonStringValue(json, "refresh_token"))
        g_cfg_refresh_token = refresh->c_str();
    const int expires = jsonIntValue(json, "expires_in").value_or(3600);
    g_cfg_token_expires_at = static_cast<int>(unixTimeSeconds() + expires - 60);
}
} // namespace

AuthManager &AuthManager::instance()
{
    static AuthManager manager;
    return manager;
}

bool AuthManager::beginLogin(std::string &message)
{
    pfc::string8 clientId = g_cfg_client_id.get();
    if (clientId.is_empty())
    {
        message = "Client ID を設定してください。";
        return false;
    }

    const std::string verifier = randomUrlSafe(64);
    const auto challenge = sha256Base64Url(verifier);
    if (!challenge)
    {
        message = "PKCE challenge を生成できませんでした。";
        return false;
    }

    const std::string state = randomUrlSafe(32);
    const std::string scope = "user-modify-playback-state user-read-playback-state playlist-read-private playlist-read-collaborative";
    const std::string authorize =
        "https://accounts.spotify.com/authorize?response_type=code&client_id=" + urlEncode(clientId.c_str()) +
        "&scope=" + urlEncode(scope) +
        "&redirect_uri=" + urlEncode(kRedirectUri) +
        "&code_challenge_method=S256&code_challenge=" + urlEncode(*challenge) +
        "&state=" + urlEncode(state);

    ShellExecuteA(core_api::get_main_window(), "open", authorize.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    const auto code = readCallbackCode(state);
    if (!code)
    {
        message = "callback を受信できませんでした。Redirect URI とポート 8088 を確認してください。";
        return false;
    }

    return exchangeCode(*code, verifier, message);
}

bool AuthManager::exchangeCode(const std::string &code, const std::string &verifier, std::string &message)
{
    pfc::string8 clientId = g_cfg_client_id.get();
    const std::string body = "client_id=" + urlEncode(clientId.c_str()) +
                             "&grant_type=authorization_code&code=" + urlEncode(code) +
                             "&redirect_uri=" + urlEncode(kRedirectUri) +
                             "&code_verifier=" + urlEncode(verifier);
    const auto response = httpRequest(L"POST", L"https://accounts.spotify.com/api/token",
                                      {L"Content-Type: application/x-www-form-urlencoded"}, body);
    if (!response)
    {
        message = "token endpoint へ接続できませんでした。";
        return false;
    }
    if (response->status < 200 || response->status >= 300)
    {
        message = "token 交換に失敗しました: HTTP " + std::to_string(response->status) + " " + response->body;
        return false;
    }

    saveTokenResponse(response->body);
    message = "Spotify login が完了しました。";
    return true;
}

bool AuthManager::refreshAccessToken(std::string &message)
{
    pfc::string8 clientId = g_cfg_client_id.get();
    pfc::string8 refresh = g_cfg_refresh_token.get();
    if (clientId.is_empty() || refresh.is_empty())
    {
        message = "Client ID または refresh token がありません。";
        return false;
    }

    const std::string body = "client_id=" + urlEncode(clientId.c_str()) +
                             "&grant_type=refresh_token&refresh_token=" + urlEncode(refresh.c_str());
    const auto response = httpRequest(L"POST", L"https://accounts.spotify.com/api/token",
                                      {L"Content-Type: application/x-www-form-urlencoded"}, body);
    if (!response || response->status < 200 || response->status >= 300)
    {
        message = "access token refresh に失敗しました。";
        return false;
    }

    saveTokenResponse(response->body);
    message = "access token を更新しました。";
    return true;
}

bool AuthManager::ensureAccessToken(std::string &token, std::string &message)
{
    pfc::string8 access = g_cfg_access_token.get();
    if (!access.is_empty() && static_cast<std::int64_t>(g_cfg_token_expires_at.get()) > unixTimeSeconds())
    {
        token = access.c_str();
        return true;
    }

    if (!refreshAccessToken(message))
        return false;

    access = g_cfg_access_token.get();
    if (access.is_empty())
    {
        message = "access token が空です。";
        return false;
    }
    token = access.c_str();
    return true;
}

void AuthManager::clearTokens()
{
    g_cfg_access_token = "";
    g_cfg_refresh_token = "";
    g_cfg_token_expires_at = 0;
}
} // namespace fsl
