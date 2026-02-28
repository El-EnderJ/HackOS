/**
 * @file sd_updater.cpp
 * @brief SD-Bootloader implementation – flash firmware from SD card.
 */

#include "core/sd_updater.h"

#include <cstring>

#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_system.h>

#include "storage/vfs.h"

static constexpr const char *TAG_UPD = "SDUpdater";

namespace hackos::core {

// ── Singleton ────────────────────────────────────────────────────────────────

SDUpdater &SDUpdater::instance()
{
    static SDUpdater upd;
    return upd;
}

SDUpdater::SDUpdater() = default;

// ── Public API ──────────────────────────────────────────────────────────────

bool SDUpdater::firmwareAvailable() const
{
    return hackos::storage::VirtualFS::instance().exists(FW_PATH);
}

size_t SDUpdater::firmwareSize() const
{
    auto &vfs = hackos::storage::VirtualFS::instance();
    fs::File f = vfs.open(FW_PATH, "r");
    if (!f)
    {
        return 0U;
    }
    const size_t sz = f.size();
    f.close();
    return sz;
}

bool SDUpdater::applyUpdate(UpdateProgressCb progressCb)
{
    auto &vfs = hackos::storage::VirtualFS::instance();
    fs::File f = vfs.open(FW_PATH, "r");
    if (!f)
    {
        ESP_LOGE(TAG_UPD, "Cannot open %s", FW_PATH);
        return false;
    }

    const size_t fileSize = f.size();
    if (fileSize == 0U)
    {
        ESP_LOGE(TAG_UPD, "Firmware file is empty");
        f.close();
        return false;
    }

    ESP_LOGI(TAG_UPD, "Firmware size: %u bytes", static_cast<unsigned>(fileSize));

    // Find the running partition and the next OTA partition.
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *target = esp_ota_get_next_update_partition(running);
    if (target == nullptr)
    {
        ESP_LOGE(TAG_UPD, "No OTA target partition found");
        f.close();
        return false;
    }

    ESP_LOGI(TAG_UPD, "Target partition: %s (offset=0x%lx, size=%lu)",
             target->label,
             static_cast<unsigned long>(target->address),
             static_cast<unsigned long>(target->size));

    if (fileSize > target->size)
    {
        ESP_LOGE(TAG_UPD, "Firmware too large for partition");
        f.close();
        return false;
    }

    esp_ota_handle_t otaHandle = 0;
    esp_err_t err = esp_ota_begin(target, fileSize, &otaHandle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG_UPD, "esp_ota_begin failed: %s", esp_err_to_name(err));
        f.close();
        return false;
    }

    uint8_t buf[READ_BUF_SIZE];
    size_t written = 0U;
    uint8_t lastPct = 0U;

    while (written < fileSize)
    {
        const size_t toRead = ((fileSize - written) < READ_BUF_SIZE)
                                  ? (fileSize - written)
                                  : READ_BUF_SIZE;
        const size_t got = f.read(buf, toRead);
        if (got == 0U)
        {
            ESP_LOGE(TAG_UPD, "Read error at offset %u", static_cast<unsigned>(written));
            esp_ota_abort(otaHandle);
            f.close();
            return false;
        }

        err = esp_ota_write(otaHandle, buf, got);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG_UPD, "esp_ota_write failed: %s", esp_err_to_name(err));
            esp_ota_abort(otaHandle);
            f.close();
            return false;
        }

        written += got;

        if (progressCb != nullptr)
        {
            const uint8_t pct = static_cast<uint8_t>((written * 100U) / fileSize);
            if (pct != lastPct)
            {
                lastPct = pct;
                progressCb(pct);
            }
        }
    }

    f.close();

    err = esp_ota_end(otaHandle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG_UPD, "esp_ota_end failed: %s", esp_err_to_name(err));
        return false;
    }

    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG_UPD, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG_UPD, "Update successful – rebooting...");
    esp_restart();
    return true; // unreachable; satisfies non-void return
}

} // namespace hackos::core
