/**
 * @file asset_loader.h
 * @brief Dynamic XBM asset loader for the HackOS UI.
 *
 * Loads monochrome bitmap assets (XBM format) from the SD card into the
 * heap on demand.  Each view can own an AssetLoader instance; when the
 * view is destroyed the loader's destructor releases all associated heap
 * memory automatically.
 *
 * On-disk binary XBM format (little-endian):
 *   [uint16_t width][uint16_t height][raw bitmap bytes …]
 *
 * The bitmap data is 1-bit-per-pixel, row-major, matching the layout
 * expected by `Adafruit_GFX::drawXBitmap()`.
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace hackos::storage {

// ── XBMAsset ─────────────────────────────────────────────────────────────────

/// @brief Heap-resident bitmap loaded from a VFS path.
struct XBMAsset
{
    uint16_t width;    ///< Image width in pixels
    uint16_t height;   ///< Image height in pixels
    uint8_t *data;     ///< Heap-allocated bitmap data (caller must NOT free)
    size_t dataSize;   ///< Size of @c data in bytes
};

// ── AssetLoader ──────────────────────────────────────────────────────────────

/**
 * @brief Per-view asset manager that loads XBM images into the heap.
 *
 * Typical usage:
 * @code
 * hackos::storage::AssetLoader loader;
 * auto *icon = loader.load("/ext/assets/wifi.xbm");
 * if (icon) display.drawXBitmap(0, 0, icon->data, icon->width, icon->height, 1);
 * // On view destroy, loader dtor calls unloadAll().
 * @endcode
 */
class AssetLoader
{
public:
    AssetLoader();

    /// @brief Destructor – releases all loaded assets.
    ~AssetLoader();

    /**
     * @brief Load an XBM asset from the VFS into the heap.
     * @param path  Virtual path (e.g. `/ext/assets/wifi.xbm`).
     * @return Pointer to the loaded asset, or nullptr on failure.
     *         The returned pointer is valid until unload() / unloadAll().
     */
    XBMAsset *load(const char *path);

    /// @brief Release a single asset's heap memory.
    void unload(XBMAsset *asset);

    /// @brief Release all loaded assets.
    void unloadAll();

    /// @brief Number of assets currently loaded.
    size_t loadedCount() const;

private:
    /// Maximum simultaneously loaded assets per loader instance.
    static constexpr size_t MAX_ASSETS = 8U;

    XBMAsset slots_[MAX_ASSETS];
    bool used_[MAX_ASSETS];
};

} // namespace hackos::storage
