/**
 * @file json_parser.h
 * @brief Ultralight JSON configuration parser for HackOS.
 *
 * Parses flat JSON objects (no nested objects or arrays) into a fixed-
 * size key→value table that can be queried by type.  Designed for small
 * application configuration files stored on the SD card or flash.
 *
 * Supported value types: strings, integers, booleans, null.
 *
 * Example config:
 * @code{.json}
 * {
 *     "name": "WiFi Scanner",
 *     "version": 2,
 *     "autostart": true
 * }
 * @endcode
 *
 * Usage:
 * @code
 * hackos::storage::JsonConfig cfg;
 * if (cfg.loadFromFile("/ext/apps/wifi/config.json")) {
 *     const char *name = cfg.getString("name");
 *     int32_t ver      = cfg.getInt("version", 1);
 *     bool autostart   = cfg.getBool("autostart");
 * }
 * @endcode
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace hackos::storage {

// ── JsonConfig ───────────────────────────────────────────────────────────────

class JsonConfig
{
public:
    /// Maximum number of key-value pairs that can be stored.
    static constexpr size_t MAX_KEYS = 16U;
    /// Maximum key length (including NUL).
    static constexpr size_t KEY_LEN = 32U;
    /// Maximum value length (including NUL).
    static constexpr size_t VAL_LEN = 64U;
    /// Maximum JSON source size that can be parsed.
    static constexpr size_t MAX_JSON_SIZE = 1024U;

    JsonConfig();

    // ── Loading ──────────────────────────────────────────────────────────

    /**
     * @brief Load and parse a JSON file from the VFS.
     * @param vfsPath  Virtual path (e.g. `/ext/apps/wifi/config.json`).
     * @return true if the file was read and parsed successfully.
     */
    bool loadFromFile(const char *vfsPath);

    /**
     * @brief Parse an in-memory JSON string.
     * @return true on success.
     */
    bool loadFromString(const char *json);

    // ── Typed accessors ──────────────────────────────────────────────────

    const char *getString(const char *key, const char *defaultVal = "") const;
    int32_t getInt(const char *key, int32_t defaultVal = 0) const;
    bool getBool(const char *key, bool defaultVal = false) const;
    bool hasKey(const char *key) const;

    /// @brief Number of parsed key-value pairs.
    size_t keyCount() const;

    /// @brief Discard all parsed data.
    void clear();

    const char *lastError() const;

private:
    /// @brief Internal key-value entry.
    struct Entry
    {
        char key[KEY_LEN];
        char value[VAL_LEN];
    };

    Entry entries_[MAX_KEYS];
    size_t count_;
    const char *lastError_;

    // ── Parser internals ─────────────────────────────────────────────────

    bool parse(const char *json);
    static const char *skipWhitespace(const char *p);
    static const char *parseString(const char *p, char *out, size_t maxLen);
    static const char *parseValue(const char *p, char *out, size_t maxLen);

    const Entry *findEntry(const char *key) const;
};

} // namespace hackos::storage
