/**
 * @file sd_updater.h
 * @brief SD-Bootloader – firmware update from SD card.
 *
 * Reads `/ext/update/firmware.bin` from the SD card and flashes it to
 * the ESP32's app0 partition using esp_ota_ops, then reboots.  This
 * makes HackOS 100% PC-independent for firmware updates.
 *
 * Usage:
 * @code
 *   auto &upd = hackos::core::SDUpdater::instance();
 *   if (upd.firmwareAvailable()) {
 *       upd.applyUpdate(myProgressCallback);
 *   }
 * @endcode
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace hackos::core {

/// @brief Progress callback: receives percent (0-100).
using UpdateProgressCb = void (*)(uint8_t percent);

class SDUpdater
{
public:
    static SDUpdater &instance();

    /// @brief Check whether `/ext/update/firmware.bin` exists.
    bool firmwareAvailable() const;

    /// @brief Get the file size of the available firmware (bytes).
    size_t firmwareSize() const;

    /**
     * @brief Flash the firmware from SD to the app0 partition.
     *
     * @param progressCb  Optional callback invoked with progress 0-100.
     * @return true on success (the device will reboot immediately).
     *         Returns false if the flash fails.
     */
    bool applyUpdate(UpdateProgressCb progressCb = nullptr);

private:
    static constexpr const char *FW_PATH = "/ext/update/firmware.bin";
    static constexpr size_t READ_BUF_SIZE = 4096U;

    SDUpdater();
};

} // namespace hackos::core
