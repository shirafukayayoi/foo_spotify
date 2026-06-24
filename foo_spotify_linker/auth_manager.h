#pragma once

namespace fsl
{
class AuthManager
{
public:
    static AuthManager &instance();

    bool beginLogin(std::string &message);
    bool ensureAccessToken(std::string &token, std::string &message);
    void clearTokens();

private:
    AuthManager() = default;

    bool exchangeCode(const std::string &code, const std::string &verifier, std::string &message);
    bool refreshAccessToken(std::string &message);
};
} // namespace fsl
