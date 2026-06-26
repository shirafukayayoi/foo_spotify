#include "stdafx.h"
#include "http_client.h"

namespace fsl
{
namespace
{
std::wstring widen(const std::string &value)
{
    if (value.empty())
        return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    std::wstring out(static_cast<size_t>(len - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, out.data(), len);
    return out;
}
} // namespace

std::string urlEncode(const std::string &value)
{
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char ch : value)
    {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~')
        {
            out.push_back(static_cast<char>(ch));
        }
        else
        {
            out.push_back('%');
            out.push_back(hex[ch >> 4]);
            out.push_back(hex[ch & 15]);
        }
    }
    return out;
}

std::optional<HttpResponse> httpRequest(const wchar_t *method,
                                        const std::wstring &url,
                                        const std::vector<std::wstring> &headers,
                                        const std::string &body)
{
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts))
        return std::nullopt;

    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    path.append(parts.lpszExtraInfo ? std::wstring(parts.lpszExtraInfo, parts.dwExtraInfoLength) : L"");

    HINTERNET session = WinHttpOpen(L"foo_spotify_linker/0.1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)
        return std::nullopt;

    HINTERNET connect = WinHttpConnect(session, host.c_str(), parts.nPort, 0);
    if (!connect)
    {
        WinHttpCloseHandle(session);
        return std::nullopt;
    }

    const DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(connect, method, path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request)
    {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return std::nullopt;
    }

    std::wstring joinedHeaders;
    for (const auto &header : headers)
    {
        joinedHeaders += header;
        joinedHeaders += L"\r\n";
    }

    const BOOL sent = WinHttpSendRequest(request,
                                         joinedHeaders.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : joinedHeaders.c_str(),
                                         static_cast<DWORD>(joinedHeaders.size()),
                                         body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(),
                                         static_cast<DWORD>(body.size()),
                                         static_cast<DWORD>(body.size()),
                                         0);
    if (!sent || !WinHttpReceiveResponse(request, nullptr))
    {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return std::nullopt;
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);

    int retryAfterSeconds = -1;
    wchar_t retryAfter[64]{};
    DWORD retryAfterSize = sizeof(retryAfter);
    if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM,
                            L"Retry-After", retryAfter, &retryAfterSize, WINHTTP_NO_HEADER_INDEX))
    {
        retryAfterSeconds = _wtoi(retryAfter);
    }

    std::string responseBody;
    for (;;)
    {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available) || available == 0)
            break;
        std::string chunk(available, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(request, chunk.data(), available, &read) || read == 0)
            break;
        chunk.resize(read);
        responseBody += chunk;
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return HttpResponse{static_cast<int>(status), responseBody, retryAfterSeconds};
}

std::optional<std::string> httpFinalUrl(const std::string &url)
{
    const std::wstring wideUrl = widen(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wideUrl.c_str(), 0, 0, &parts))
        return std::nullopt;

    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    path.append(parts.lpszExtraInfo ? std::wstring(parts.lpszExtraInfo, parts.dwExtraInfoLength) : L"");

    HINTERNET session = WinHttpOpen(L"foo_spotify_linker/0.1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)
        return std::nullopt;
    HINTERNET connect = WinHttpConnect(session, host.c_str(), parts.nPort, 0);
    if (!connect)
    {
        WinHttpCloseHandle(session);
        return std::nullopt;
    }
    const DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(connect, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request)
    {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return std::nullopt;
    }
    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, nullptr))
    {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return std::nullopt;
    }

    DWORD size = 0;
    WinHttpQueryOption(request, WINHTTP_OPTION_URL, nullptr, &size);
    std::wstring finalUrl(size / sizeof(wchar_t), L'\0');
    if (!finalUrl.empty() && WinHttpQueryOption(request, WINHTTP_OPTION_URL, finalUrl.data(), &size))
    {
        if (!finalUrl.empty() && finalUrl.back() == L'\0')
            finalUrl.pop_back();
    }
    else
    {
        finalUrl = wideUrl;
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return pfc::stringcvt::string_utf8_from_wide(finalUrl.c_str()).get_ptr();
}

std::optional<std::string> jsonStringValue(const std::string &json, const std::string &key)
{
    const std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos)
        return std::nullopt;
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos)
        return std::nullopt;
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos)
        return std::nullopt;
    std::string out;
    bool escape = false;
    for (++pos; pos < json.size(); ++pos)
    {
        const char ch = json[pos];
        if (escape)
        {
            out.push_back(ch);
            escape = false;
            continue;
        }
        if (ch == '\\')
        {
            escape = true;
            continue;
        }
        if (ch == '"')
            return out;
        out.push_back(ch);
    }
    return std::nullopt;
}

std::optional<int> jsonIntValue(const std::string &json, const std::string &key)
{
    const std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos)
        return std::nullopt;
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos)
        return std::nullopt;
    ++pos;
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos])))
        ++pos;
    int value = 0;
    bool any = false;
    while (pos < json.size() && std::isdigit(static_cast<unsigned char>(json[pos])))
    {
        value = value * 10 + (json[pos] - '0');
        any = true;
        ++pos;
    }
    if (!any)
        return std::nullopt;
    return value;
}

std::int64_t unixTimeSeconds()
{
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
} // namespace fsl
