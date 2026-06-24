#include "stdafx.h"
#include "mapping_manager.h"

namespace fsl
{
namespace
{
std::int64_t nowMs()
{
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}
} // namespace

MappingManager &MappingManager::instance()
{
    static MappingManager manager;
    return manager;
}

bool MappingManager::open(const std::string &path)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_db)
        return true;

    if (sqlite3_open(path.c_str(), &m_db) != SQLITE_OK)
    {
        FB2K_console_formatter() << "foo_spotify_linker: SQLite を開けません: " << path.c_str();
        close();
        return false;
    }

    return exec("CREATE TABLE IF NOT EXISTS track_map ("
                "local_hash TEXT PRIMARY KEY,"
                "spotify_uri TEXT NOT NULL,"
                "updated_at INTEGER NOT NULL)") &&
           exec("CREATE TABLE IF NOT EXISTS album_map ("
                "album_id TEXT PRIMARY KEY,"
                "spotify_album_uri TEXT NOT NULL,"
                "updated_at INTEGER NOT NULL)") &&
           exec("CREATE TABLE IF NOT EXISTS config ("
                "key TEXT PRIMARY KEY,"
                "value TEXT NOT NULL)");
}

void MappingManager::close()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_db)
    {
        sqlite3_close(m_db);
        m_db = nullptr;
    }
}

bool MappingManager::isOpen() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_db != nullptr;
}

bool MappingManager::exec(const char *sql)
{
    char *error = nullptr;
    const int rc = sqlite3_exec(m_db, sql, nullptr, nullptr, &error);
    if (rc != SQLITE_OK)
    {
        FB2K_console_formatter() << "foo_spotify_linker: SQLite エラー: " << (error ? error : "(unknown)");
        sqlite3_free(error);
        return false;
    }
    return true;
}

std::optional<std::string> MappingManager::resolve(const TrackMetadata &metadata)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_db)
        return std::nullopt;

    const std::string localHash = makeLocalHash(metadata);
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, "SELECT spotify_uri FROM track_map WHERE local_hash = ?", -1, &stmt, nullptr) != SQLITE_OK)
        return std::nullopt;

    sqlite3_bind_text(stmt, 1, localHash.c_str(), -1, SQLITE_TRANSIENT);
    std::optional<std::string> result;
    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        const unsigned char *text = sqlite3_column_text(stmt, 0);
        if (text)
            result = reinterpret_cast<const char *>(text);
    }
    sqlite3_finalize(stmt);
    return result;
}

bool MappingManager::addTrackMapping(const std::string &localHash, const std::string &spotifyUri)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_db)
        return false;

    sqlite3_stmt *stmt = nullptr;
    const char *sql =
        "INSERT INTO track_map(local_hash, spotify_uri, updated_at) VALUES(?, ?, ?) "
        "ON CONFLICT(local_hash) DO UPDATE SET spotify_uri = excluded.spotify_uri, updated_at = excluded.updated_at";
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    sqlite3_bind_text(stmt, 1, localHash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, spotifyUri.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, nowMs());
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

bool MappingManager::removeTrackMapping(const std::string &localHash)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_db)
        return false;

    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, "DELETE FROM track_map WHERE local_hash = ?", -1, &stmt, nullptr) != SQLITE_OK)
        return false;
    sqlite3_bind_text(stmt, 1, localHash.c_str(), -1, SQLITE_TRANSIENT);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

bool MappingManager::setConfig(const std::string &key, const std::string &value)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_db)
        return false;

    sqlite3_stmt *stmt = nullptr;
    const char *sql = "INSERT INTO config(key, value) VALUES(?, ?) "
                      "ON CONFLICT(key) DO UPDATE SET value = excluded.value";
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;
    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, value.c_str(), -1, SQLITE_TRANSIENT);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

std::string MappingManager::getConfig(const std::string &key, const std::string &fallback)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_db)
        return fallback;

    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, "SELECT value FROM config WHERE key = ?", -1, &stmt, nullptr) != SQLITE_OK)
        return fallback;
    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    std::string value = fallback;
    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        const unsigned char *text = sqlite3_column_text(stmt, 0);
        if (text)
            value = reinterpret_cast<const char *>(text);
    }
    sqlite3_finalize(stmt);
    return value;
}
} // namespace fsl
