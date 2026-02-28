/**
 * @file cloud_exfil_app.cpp
 * @brief Cloud Exfiltration config UI – view/test Telegram/Discord webhooks.
 *
 * Shows the current webhook configuration status and allows the user
 * to reload the config from `/ext/cloud.cfg` or send a test message.
 */

#include "apps/cloud_exfil_app.h"

#include <cstdio>
#include <cstring>
#include <new>

#include <esp_log.h>

#include "core/event.h"
#include "core/event_system.h"
#include "hardware/display.h"
#include "hardware/input.h"
#include "net/cloud_exfil.h"
#include "ui/toast_manager.h"

static constexpr const char *TAG_CEA = "CloudExfilApp";

namespace
{

static constexpr size_t MENU_ITEMS = 3U;
static const char *const MENU_LABELS[MENU_ITEMS] = {
    "Reload Config",
    "Send Test Msg",
    "Back",
};

class CloudExfilAppImpl final : public AppBase
{
public:
    CloudExfilAppImpl() : sel_(0U) {}

    void onSetup() override
    {
        hackos::net::CloudExfil::instance().loadConfig();
    }

    void onLoop() override {}

    void onDraw() override
    {
        auto &d = DisplayManager::instance();
        auto &cloud = hackos::net::CloudExfil::instance();
        d.clear();

        d.drawText(0, 0, "Cloud Exfil", 1U);
        d.drawLine(0, 10, 127, 10);

        const char *typeStr = "None";
        if (cloud.type() == hackos::net::WebhookType::TELEGRAM)
        {
            typeStr = "Telegram";
        }
        else if (cloud.type() == hackos::net::WebhookType::DISCORD)
        {
            typeStr = "Discord";
        }

        char statusBuf[32];
        std::snprintf(statusBuf, sizeof(statusBuf), "Type: %s", typeStr);
        d.drawText(0, 13, statusBuf);

        d.drawText(0, 23, cloud.isConfigured() ? "Status: OK" : "Status: NOT SET");

        // Menu
        for (size_t i = 0U; i < MENU_ITEMS; ++i)
        {
            const int16_t y = static_cast<int16_t>(36 + i * 10);
            if (i == sel_)
            {
                d.fillRect(0, y, 128, 10, SSD1306_WHITE);
                d.drawText(2, y + 1, MENU_LABELS[i], 1U, SSD1306_BLACK);
            }
            else
            {
                d.drawText(2, y + 1, MENU_LABELS[i], 1U, SSD1306_WHITE);
            }
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

        switch (input)
        {
        case InputManager::InputEvent::UP:
            if (sel_ > 0U)
            {
                --sel_;
            }
            break;
        case InputManager::InputEvent::DOWN:
            if (sel_ + 1U < MENU_ITEMS)
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
    void handleAction()
    {
        auto &cloud = hackos::net::CloudExfil::instance();

        switch (sel_)
        {
        case 0U: // Reload
            if (cloud.loadConfig())
            {
                ToastManager::instance().show("[+] Config loaded");
            }
            else
            {
                ToastManager::instance().show("[!] Config error");
            }
            break;
        case 1U: // Test
            if (cloud.send("HackOS test message"))
            {
                ToastManager::instance().show("[+] Message sent");
            }
            else
            {
                ToastManager::instance().show("[!] Send failed");
            }
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

    size_t sel_;
};

} // anonymous namespace

AppBase *createCloudExfilApp()
{
    return new (std::nothrow) CloudExfilAppImpl();
}
