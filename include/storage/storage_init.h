/**
 * @file storage_init.h
 * @brief First-boot directory structure verification for the SD card.
 *
 * Ensures that the mandatory folder hierarchy exists on the external
 * SD card.  Missing directories are created automatically so that
 * applications can safely assume the structure is in place.
 *
 * Required directories (under `/ext`):
 *  - `/ext/apps`      – application bundles
 *  - `/ext/payloads`  – BadUSB / EvilTwin payloads
 *  - `/ext/captures`  – PCAP / RF raw captures
 *  - `/ext/assets`    – icons, images (XBM)
 */

#pragma once

namespace hackos::storage {

// ── StorageInit ──────────────────────────────────────────────────────────────

class StorageInit
{
public:
    /**
     * @brief Verify and create the mandatory SD card folder structure.
     *
     * Iterates over REQUIRED_DIRS and creates any directory that does
     * not yet exist.  Requires the VirtualFS to be initialised first.
     *
     * @return true if all directories exist (or were created successfully).
     */
    static bool ensureFolderStructure();

private:
    /// Number of required directories.
    static constexpr size_t DIR_COUNT = 4U;

    /// Paths that must exist on the SD card (VFS virtual paths).
    static const char *const REQUIRED_DIRS[DIR_COUNT];
};

} // namespace hackos::storage
