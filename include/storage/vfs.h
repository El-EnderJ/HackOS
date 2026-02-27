/**
 * @file vfs.h
 * @brief Virtual File System – unified access to SD card and internal flash.
 *
 * The VirtualFS singleton routes file operations through a path prefix:
 *  - `/ext/…` → SD card (FAT32, via the Arduino SD library).
 *  - `/int/…` → Internal flash (LittleFS partition).
 *
 * Callers interact with a single API regardless of the underlying storage
 * backend, keeping application code storage-agnostic.
 *
 * @code
 * auto &vfs = hackos::storage::VirtualFS::instance();
 * fs::File f = vfs.open("/ext/payloads/attack.txt", "r");
 * if (f) { // use f … f.close(); }
 * @endcode
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <FS.h>

namespace hackos::storage {

// ── Storage back-end identifier ──────────────────────────────────────────────

/// @brief Identifies which physical storage a path resolves to.
enum class StorageType : uint8_t
{
    SD_CARD, ///< External SD card (`/ext/…`)
    FLASH,   ///< Internal LittleFS flash (`/int/…`)
    UNKNOWN, ///< Path prefix not recognised
};

// ── VirtualFS ────────────────────────────────────────────────────────────────

/**
 * @brief Singleton that multiplexes file operations across SD and LittleFS.
 */
class VirtualFS
{
public:
    static VirtualFS &instance();

    /**
     * @brief Mount both storage back-ends (SD + LittleFS).
     *
     * SD mounting is skipped if the StorageManager has already mounted it.
     * LittleFS is formatted on first use if the partition is empty.
     *
     * @return true if at least one back-end is available.
     */
    bool init();

    // ── Unified file operations ──────────────────────────────────────────

    /**
     * @brief Open a file using a virtual path.
     * @param path  Virtual path (must start with `/ext/` or `/int/`).
     * @param mode  Arduino file-open mode string (`"r"`, `"w"`, `"a"`).
     * @return An open `fs::File`, or an invalid File (operator bool == false).
     */
    fs::File open(const char *path, const char *mode = "r");

    /// @brief Check whether a file or directory exists.
    bool exists(const char *path);

    /// @brief Create a directory (and parents where the FS supports it).
    bool mkdir(const char *path);

    /// @brief Delete a file.
    bool remove(const char *path);

    // ── Directory listing ────────────────────────────────────────────────

    /// @brief Metadata for a single directory entry.
    struct DirEntry
    {
        char name[64]; ///< Null-terminated short name
        bool isDir;    ///< true if the entry is a sub-directory
        uint32_t size; ///< File size in bytes (0 for directories)
    };

    /**
     * @brief List directory entries at @p path.
     * @return Number of entries written, or 0 on error.
     */
    size_t listDir(const char *path, DirEntry *entries, size_t maxEntries);

    // ── Status ───────────────────────────────────────────────────────────

    bool sdMounted() const;
    bool flashMounted() const;
    const char *lastError() const;

    // ── Path helpers (public for testability) ────────────────────────────

    /// @brief Determine which back-end a virtual path maps to.
    static StorageType resolveStorage(const char *path);

    /// @brief Strip the `/ext` or `/int` prefix, returning the FS-local path.
    static const char *stripPrefix(const char *path);

private:
    VirtualFS();

    /// @brief Return the `fs::FS` reference for the given back-end.
    fs::FS *getFS(StorageType type);

    bool sdMounted_;
    bool flashMounted_;
    const char *lastError_;
};

} // namespace hackos::storage
