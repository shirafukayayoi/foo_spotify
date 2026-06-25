#pragma once

#include "metadata.h"

namespace fsl
{
class MappingManager
{
public:
    static MappingManager &instance();

    bool open(const std::string &path);
    void close();
    bool isOpen() const;

    std::optional<std::string> resolve(const TrackMetadata &metadata);
    std::optional<std::string> getTrackMapping(const std::string &localHash);
    bool addTrackMapping(const std::string &localHash, const std::string &spotifyUri);
    bool removeTrackMapping(const std::string &localHash);
    bool setConfig(const std::string &key, const std::string &value);
    std::string getConfig(const std::string &key, const std::string &fallback = "");

private:
    MappingManager() = default;
    bool exec(const char *sql);

    sqlite3 *m_db = nullptr;
    mutable std::mutex m_mutex;
};
} // namespace fsl
