#include "storage/asset_loader.h"

#include <cstring>
#include <esp_log.h>

#include "storage/vfs.h"

static constexpr const char *TAG_ASSET = "AssetLoader";

namespace hackos::storage {

// ── Construction / destruction ───────────────────────────────────────────────

AssetLoader::AssetLoader()
{
    for (size_t i = 0U; i < MAX_ASSETS; ++i)
    {
        slots_[i] = {0U, 0U, nullptr, 0U};
        used_[i] = false;
    }
}

AssetLoader::~AssetLoader()
{
    unloadAll();
}

// ── Loading ──────────────────────────────────────────────────────────────────

XBMAsset *AssetLoader::load(const char *path)
{
    if (path == nullptr)
    {
        return nullptr;
    }

    // Find a free slot.
    size_t slot = MAX_ASSETS;
    for (size_t i = 0U; i < MAX_ASSETS; ++i)
    {
        if (!used_[i])
        {
            slot = i;
            break;
        }
    }
    if (slot == MAX_ASSETS)
    {
        ESP_LOGW(TAG_ASSET, "load: no free slot");
        return nullptr;
    }

    // Open the file through the VFS.
    fs::File f = VirtualFS::instance().open(path, "r");
    if (!f)
    {
        ESP_LOGW(TAG_ASSET, "load: cannot open %s", path);
        return nullptr;
    }

    // Read the 4-byte header (width + height, little-endian uint16).
    uint8_t header[4];
    if (f.read(header, 4U) != 4U)
    {
        ESP_LOGW(TAG_ASSET, "load: header read failed");
        f.close();
        return nullptr;
    }

    const uint16_t width = static_cast<uint16_t>(header[0] | (header[1] << 8U));
    const uint16_t height = static_cast<uint16_t>(header[2] | (header[3] << 8U));

    if (width == 0U || height == 0U)
    {
        ESP_LOGW(TAG_ASSET, "load: invalid dimensions %ux%u", width, height);
        f.close();
        return nullptr;
    }

    // Bitmap size: 1 bit per pixel, row-major.
    const size_t dataSize = (static_cast<size_t>(width) + 7U) / 8U *
                            static_cast<size_t>(height);

    uint8_t *data = new (std::nothrow) uint8_t[dataSize];
    if (data == nullptr)
    {
        ESP_LOGE(TAG_ASSET, "load: heap alloc failed (%u bytes)",
                 static_cast<unsigned>(dataSize));
        f.close();
        return nullptr;
    }

    const size_t bytesRead = f.read(data, dataSize);
    f.close();

    if (bytesRead != dataSize)
    {
        ESP_LOGW(TAG_ASSET, "load: short read (%u/%u)",
                 static_cast<unsigned>(bytesRead),
                 static_cast<unsigned>(dataSize));
        delete[] data;
        return nullptr;
    }

    slots_[slot].width = width;
    slots_[slot].height = height;
    slots_[slot].data = data;
    slots_[slot].dataSize = dataSize;
    used_[slot] = true;

    ESP_LOGI(TAG_ASSET, "loaded %s (%ux%u, %u bytes) → slot %u",
             path, width, height, static_cast<unsigned>(dataSize),
             static_cast<unsigned>(slot));
    return &slots_[slot];
}

// ── Unloading ────────────────────────────────────────────────────────────────

void AssetLoader::unload(XBMAsset *asset)
{
    if (asset == nullptr)
    {
        return;
    }
    for (size_t i = 0U; i < MAX_ASSETS; ++i)
    {
        if (used_[i] && &slots_[i] == asset)
        {
            delete[] slots_[i].data;
            slots_[i] = {0U, 0U, nullptr, 0U};
            used_[i] = false;
            return;
        }
    }
}

void AssetLoader::unloadAll()
{
    for (size_t i = 0U; i < MAX_ASSETS; ++i)
    {
        if (used_[i])
        {
            delete[] slots_[i].data;
            slots_[i] = {0U, 0U, nullptr, 0U};
            used_[i] = false;
        }
    }
}

size_t AssetLoader::loadedCount() const
{
    size_t n = 0U;
    for (size_t i = 0U; i < MAX_ASSETS; ++i)
    {
        if (used_[i])
        {
            ++n;
        }
    }
    return n;
}

} // namespace hackos::storage
