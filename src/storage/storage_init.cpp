#include "storage/storage_init.h"

#include <esp_log.h>

#include "storage/vfs.h"

static constexpr const char *TAG_INIT = "StorageInit";

namespace hackos::storage {

// ── Required directories (VFS paths) ─────────────────────────────────────────

const char *const StorageInit::REQUIRED_DIRS[DIR_COUNT] = {
    "/ext/apps",
    "/ext/payloads",
    "/ext/captures",
    "/ext/assets",
    "/ext/portals",
    "/ext/assets/ir",
    "/ext/assets/ir/saved",
    "/ext/nfc",
    "/ext/nfc/amiibo",
    "/ext/badbt",
    "/ext/ghostnet",
    "/ext/dashboard",
    "/ext/plugins",
    "/ext/pwnmode",
    "/ext/pcap",
    "/ext/hwbridge",
    "/ext/assets/subghz",
    "/ext/scripts",
    "/ext/update",
};

// ── Public API ───────────────────────────────────────────────────────────────

bool StorageInit::ensureFolderStructure()
{
    auto &vfs = VirtualFS::instance();

    if (!vfs.sdMounted())
    {
        ESP_LOGW(TAG_INIT, "SD not mounted – skipping folder check");
        return false;
    }

    bool allOk = true;
    for (size_t i = 0U; i < DIR_COUNT; ++i)
    {
        if (vfs.exists(REQUIRED_DIRS[i]))
        {
            ESP_LOGI(TAG_INIT, "  ✓ %s", REQUIRED_DIRS[i]);
        }
        else
        {
            if (vfs.mkdir(REQUIRED_DIRS[i]))
            {
                ESP_LOGI(TAG_INIT, "  + created %s", REQUIRED_DIRS[i]);
            }
            else
            {
                ESP_LOGE(TAG_INIT, "  ✗ failed to create %s", REQUIRED_DIRS[i]);
                allOk = false;
            }
        }
    }

    return allOk;
}

} // namespace hackos::storage
