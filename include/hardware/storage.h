#pragma once

#include <cstddef>
#include <cstdint>

class StorageManager
{
public:
    static StorageManager &instance();

    /// @brief Metadata for a single SD directory entry.
    struct DirEntry
    {
        char name[64]; ///< Null-terminated short name
        bool isDir;    ///< true if this entry is a sub-directory
        uint32_t size; ///< File size in bytes (0 for directories)
    };

    bool mount();
    void unmount();
    bool isMounted() const;
    const char *lastError() const;

    /**
     * @brief List directory entries.
     * @param path       Absolute path to the directory (e.g. "/").
     * @param entries    Output buffer for results.
     * @param maxEntries Capacity of the output buffer.
     * @return Number of entries written to @p entries (0 on error).
     */
    size_t listDir(const char *path, DirEntry *entries, size_t maxEntries);

    /**
     * @brief Write (overwrite) a file on the SD card.
     *
     * Uses synchronous SD writes.  Keep @p len reasonably small (< 4 KB) to
     * avoid blocking the UI for perceptible durations.
     *
     * @return true on success.
     */
    bool writeFile(const char *path, const uint8_t *data, size_t len);

    /**
     * @brief Append a chunk to an existing file (or create it).
     *
     * Designed for iterative / chunked writes so the caller can yield between
     * chunks and keep the UI responsive.
     *
     * @return true on success.
     */
    bool appendChunk(const char *path, const uint8_t *data, size_t len);

private:
    StorageManager();

    bool mounted_;
    const char *lastError_;
};
