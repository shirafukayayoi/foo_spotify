#pragma once

namespace fsl
{
struct HttpResponse
{
    int status = 0;
    std::string body;
    int retryAfterSeconds = -1;
};

std::string urlEncode(const std::string &value);
std::optional<HttpResponse> httpRequest(const wchar_t *method,
                                        const std::wstring &url,
                                        const std::vector<std::wstring> &headers,
                                        const std::string &body);
std::optional<std::string> httpFinalUrl(const std::string &url);
std::optional<std::string> jsonStringValue(const std::string &json, const std::string &key);
std::optional<int> jsonIntValue(const std::string &json, const std::string &key);
std::int64_t unixTimeSeconds();
} // namespace fsl
