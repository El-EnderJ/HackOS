/**
 * @file sd_updater_app.cpp
 * @brief SD-Bootloader UI – check for and apply firmware updates from SD.
 *
 * Scans `/ext/update/firmware.bin` for a new firmware image and allows
 * the user to flash it in-place via esp_ota_ops with a progress bar.
 */

#include "apps/sd_updater_app.h"

#include <cstdio>
#include <cstring>
#include <new>

#include <esp_log.h>

#include "core/event.h"
#include "core/event_system.h"
#include "core/sd_updater.h"
#include "hardware/display.h"
#include "hardware/input.h"
#include "ui/toast_manager.h"

static constexpr const char *TAG_SUA = "SDUpdaterApp";

namespace
{

enum class UPDView : uint8_t
{
    STATUS,
    FLASHING,
    DONE,
};

/// Global progress value shared with the OTA callback.
static volatile uint8_t s_updProgress = 0U;

static void otaProgressCb(uint8_t pct)
{
    s_updProgress = pct;
}

class SDUpdaterAppImpl final : public AppBase
{
public:
    SDUpdaterAppImpl()
        : view_(UPDView::STATUS),
          sel_(0U),
          fwAvailable_(false),
          fwSize_(0U)
    {
    }

    void onSetup() override
    {
        checkFirmware();
    }

    void onLoop() override {}

    void onDraw() override
    {
        auto &d = DisplayManager::instance();
        d.clear();

        switch (view_)
        {
        case UPDView::STATUS:
            drawStatus(d);
            break;
        case UPDView::FLASHING:
            drawFlashing(d);
            break;
        case UPDView::DONE:
            d.drawText(0, 20, "Update complete!");
            d.drawText(0, 34, "Rebooting...");
            break;
        }

        d.present();
    }

    void onEvent(Event *event) override
    {
        if (event == nullptr || event->type != EventType::EVT_INPUT)
        {
            return;
        }

        const auto input = static_cast<InputManager::InputEvent>(event->arg0);

        if (view_ == UPDView::FLASHING || view_ == UPDView::DONE)
        {
            return; // No interaction during flash
        }

        switch (input)
        {
        case InputManager::InputEvent::UP:
            if (sel_ > 0U)
            {
                --sel_;
            }
            break;
        case InputManager::InputEvent::DOWN:
            if (sel_ < 2U)
            {
                ++sel_;
            }
            break;
        case InputManager::InputEvent::BUTTON_PRESS:
            handleAction();
            break;
        case InputManager::InputEvent::LEFT:
        {
            Event back{EventType::EVT_SYSTEM, SYSTEM_EVENT_BACK, 0, nullptr};
            (void)EventSystem::instance().postEvent(back);
            break;
        }
        default:
            break;
        }
    }

    void onDestroy() override {}

private:
    void checkFirmware()
    {
        auto &upd = hackos::core::SDUpdater::instance();
        fwAvailable_ = upd.firmwareAvailable();
        fwSize_ = upd.firmwareSize();
        ESP_LOGI(TAG_SUA, "Firmware available: %s (%u bytes)",
                 fwAvailable_ ? "yes" : "no", static_cast<unsigned>(fwSize_));
    }

    void drawStatus(DisplayManager &d)
    {
        d.drawText(0, 0, "SD Updater", 1U);
        d.drawLine(0, 10, 127, 10);

        if (fwAvailable_)
        {
            d.drawText(0, 13, "Firmware found!");
            char sizeBuf[24];
            std::snprintf(sizeBuf, sizeof(sizeBuf), "Size: %u KB",
                          static_cast<unsigned>(fwSize_ / 1024U));
            d.drawText(0, 23, sizeBuf);
        }
        else
        {
            d.drawText(0, 13, "No firmware in");
            d.drawText(0, 23, "/ext/update/");
        }

        static const char *const labels[] = {"Flash Now", "Refresh", "Back"};
        for (size_t i = 0U; i < 3U; ++i)
        {
            const int16_t y = static_cast<int16_t>(36 + i * 10);
            if (i == sel_)
            {
                d.fillRect(0, y, 128, 10, SSD1306_WHITE);
                d.drawText(2, y + 1, labels[i], 1U, SSD1306_BLACK);
            }
            else
            {
                d.drawText(2, y + 1, labels[i], 1U, SSD1306_WHITE);
            }
        }
    }

    void drawFlashing(DisplayManager &d)
    {
        d.drawText(0, 0, "Flashing...", 1U);
        d.drawLine(0, 10, 127, 10);

        d.drawText(0, 16, "DO NOT POWER OFF!");

        char pctBuf[8];
        std::snprintf(pctBuf, sizeof(pctBuf), "%u%%", static_cast<unsigned>(s_updProgress));
        d.drawText(54, 30, pctBuf);

        // Progress bar
        d.drawRect(0, 44, 128, 12);
        const int16_t barW = static_cast<int16_t>((s_updProgress * 124) / 100);
        d.fillRect(2, 46, barW, 8);
    }

    void handleAction()
    {
        switch (sel_)
        {
        case 0U: // Flash
            if (!fwAvailable_)
            {
                ToastManager::instance().show("[!] No firmware file");
                return;
            }
            view_ = UPDView::FLASHING;
            s_updProgress = 0U;
            // Note: applyUpdate blocks and reboots on success.
            if (!hackos::core::SDUpdater::instance().applyUpdate(otaProgressCb))
            {
                view_ = UPDView::STATUS;
                ToastManager::instance().show("[!] Update failed");
            }
            break;
        case 1U: // Refresh
            checkFirmware();
            ToastManager::instance().show("[+] Refreshed");
            break;
        case 2U: // Back
        {
            Event back{EventType::EVT_SYSTEM, SYSTEM_EVENT_BACK, 0, nullptr};
            (void)EventSystem::instance().postEvent(back);
            break;
        }
        default:
            break;
        }
    }

    UPDView view_;
    size_t sel_;
    bool fwAvailable_;
    size_t fwSize_;
};

} // anonymous namespace

AppBase *createSDUpdaterApp()
{
    return new (std::nothrow) SDUpdaterAppImpl();
}
