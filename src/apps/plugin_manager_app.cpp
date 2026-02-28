/**
 * @file plugin_manager_app.cpp
 * @brief Plugin Manager App – browse, enable/disable, and manage plugins.
 *
 * Provides an on-device OLED UI for managing loaded plugins:
 *  - View list of installed plugins
 *  - See plugin details (version, author, description)
 *  - Enable/disable plugins
 *  - Reload plugins from SD card
 */

#include "apps/plugin_manager_app.h"

#include <cstdio>
#include <cstring>
#include <new>

#include "core/app_manager.h"
#include "core/event.h"
#include "core/event_system.h"
#include "core/plugin_manager.h"
#include "hardware/display.h"
#include "hardware/input.h"

namespace
{

static constexpr size_t MENU_COUNT = 4U;
static const char *const MAIN_MENU_LABELS[MENU_COUNT] = {
    "Installed Plugins",
    "Reload Plugins",
    "Plugin Info",
    "Back",
};

class PluginManagerApp final : public AppBase, public IEventObserver
{
public:
    enum class UiState : uint8_t
    {
        MAIN_MENU,
        PLUGIN_LIST,
        PLUGIN_DETAIL,
        RELOAD_RESULT,
    };

    PluginManagerApp() = default;

    void onSetup() override
    {
        state_ = UiState::MAIN_MENU;
        menuSel_ = 0U;
        listSel_ = 0U;
        (void)EventSystem::instance().subscribe(this);
    }

    void onLoop() override {}

    void onDraw() override
    {
        auto &disp = DisplayManager::instance();
        disp.clear();

        switch (state_)
        {
        case UiState::MAIN_MENU:
            drawMainMenu(disp);
            break;
        case UiState::PLUGIN_LIST:
            drawPluginList(disp);
            break;
        case UiState::PLUGIN_DETAIL:
            drawPluginDetail(disp);
            break;
        case UiState::RELOAD_RESULT:
            drawReloadResult(disp);
            break;
        }

        disp.present();
    }

    void onEvent(Event *event) override
    {
        if (event == nullptr || event->type != EventType::EVT_INPUT)
        {
            return;
        }

        const auto input = static_cast<InputManager::InputEvent>(event->arg0);

        switch (state_)
        {
        case UiState::MAIN_MENU:
            handleMainMenu(input);
            break;
        case UiState::PLUGIN_LIST:
            handlePluginList(input);
            break;
        case UiState::PLUGIN_DETAIL:
            handlePluginDetail(input);
            break;
        case UiState::RELOAD_RESULT:
            handleReloadResult(input);
            break;
        }
    }

    void onDestroy() override
    {
        EventSystem::instance().unsubscribe(this);
    }

private:
    // ── Drawing ──────────────────────────────────────────────────────────

    void drawMainMenu(DisplayManager &disp)
    {
        disp.drawText(0, 0, "Plugin Manager");
        disp.drawLine(0, 10, 127, 10);

        auto &pm = hackos::core::PluginManager::instance();
        char countBuf[32];
        snprintf(countBuf, sizeof(countBuf), "%u plugins loaded",
                 static_cast<unsigned>(pm.pluginCount()));
        disp.drawText(0, 12, countBuf);

        for (size_t i = 0U; i < MENU_COUNT; ++i)
        {
            const int y = 24 + static_cast<int>(i) * 10;
            if (i == menuSel_)
            {
                disp.fillRect(0, y - 1, 128, 9);
                disp.drawText(2, y, MAIN_MENU_LABELS[i], 1U, 0U);
            }
            else
            {
                disp.drawText(2, y, MAIN_MENU_LABELS[i]);
            }
        }
    }

    void drawPluginList(DisplayManager &disp)
    {
        disp.drawText(0, 0, "Installed Plugins");
        disp.drawLine(0, 10, 127, 10);

        auto &pm = hackos::core::PluginManager::instance();
        const size_t total = pm.pluginCount();

        if (total == 0U)
        {
            disp.drawText(0, 16, "No plugins found");
            disp.drawText(0, 28, "Add .json files to");
            disp.drawText(0, 38, "/ext/plugins/");
            return;
        }

        const size_t maxVisible = 4U;
        size_t firstVisible = 0U;
        if (listSel_ >= maxVisible)
        {
            firstVisible = listSel_ - maxVisible + 1U;
        }

        for (size_t i = 0U; i < maxVisible && (firstVisible + i) < total; ++i)
        {
            const size_t idx = firstVisible + i;
            const auto *info = pm.pluginAt(idx);
            if (info == nullptr)
            {
                continue;
            }

            char line[48];
            snprintf(line, sizeof(line), "%s%s",
                     info->enabled ? "[+] " : "[-] ", info->label);

            const int y = 14 + static_cast<int>(i) * 10;
            if (idx == listSel_)
            {
                disp.fillRect(0, y - 1, 128, 9);
                disp.drawText(2, y, line, 1U, 0U);
            }
            else
            {
                disp.drawText(2, y, line);
            }
        }

        char nav[24];
        snprintf(nav, sizeof(nav), "%u/%u",
                 static_cast<unsigned>(listSel_ + 1U),
                 static_cast<unsigned>(total));
        disp.drawText(96, 56, nav);
    }

    void drawPluginDetail(DisplayManager &disp)
    {
        auto &pm = hackos::core::PluginManager::instance();
        const auto *info = pm.pluginAt(listSel_);
        if (info == nullptr)
        {
            disp.drawText(0, 0, "Error");
            return;
        }

        disp.drawText(0, 0, info->label);
        disp.drawLine(0, 10, 127, 10);

        char buf[64];
        snprintf(buf, sizeof(buf), "v%s by %s", info->version, info->author);
        disp.drawText(0, 14, buf);
        disp.drawText(0, 26, info->description);

        snprintf(buf, sizeof(buf), "Actions: %u", static_cast<unsigned>(info->actionCount));
        disp.drawText(0, 38, buf);

        snprintf(buf, sizeof(buf), "Status: %s", info->enabled ? "Enabled" : "Disabled");
        disp.drawText(0, 48, buf);

        disp.drawText(0, 58, "[Press] toggle [Back]");
    }

    void drawReloadResult(DisplayManager &disp)
    {
        disp.drawText(0, 0, "Reload Complete");
        disp.drawLine(0, 10, 127, 10);

        char buf[48];
        snprintf(buf, sizeof(buf), "Loaded: %u plugins", reloadCount_);
        disp.drawText(0, 20, buf);

        auto &pm = hackos::core::PluginManager::instance();
        snprintf(buf, sizeof(buf), "Total: %u plugins",
                 static_cast<unsigned>(pm.pluginCount()));
        disp.drawText(0, 32, buf);

        disp.drawText(0, 48, "[Press] to continue");
    }

    // ── Input handlers ───────────────────────────────────────────────────

    void handleMainMenu(InputManager::InputEvent input)
    {
        if (input == InputManager::InputEvent::UP && menuSel_ > 0U)
        {
            --menuSel_;
        }
        else if (input == InputManager::InputEvent::DOWN && menuSel_ < MENU_COUNT - 1U)
        {
            ++menuSel_;
        }
        else if (input == InputManager::InputEvent::BUTTON_PRESS)
        {
            switch (menuSel_)
            {
            case 0U:
                state_ = UiState::PLUGIN_LIST;
                listSel_ = 0U;
                break;
            case 1U:
            {
                auto &pm = hackos::core::PluginManager::instance();
                reloadCount_ = static_cast<unsigned>(pm.reload());
                state_ = UiState::RELOAD_RESULT;
                break;
            }
            case 2U:
                if (hackos::core::PluginManager::instance().pluginCount() > 0U)
                {
                    listSel_ = 0U;
                    state_ = UiState::PLUGIN_DETAIL;
                }
                break;
            case 3U:
                EventSystem::instance().postEvent(
                    {EventType::EVT_SYSTEM, SYSTEM_EVENT_BACK, 0, nullptr});
                break;
            }
        }
    }

    void handlePluginList(InputManager::InputEvent input)
    {
        auto &pm = hackos::core::PluginManager::instance();
        const size_t total = pm.pluginCount();

        if (input == InputManager::InputEvent::UP && listSel_ > 0U)
        {
            --listSel_;
        }
        else if (input == InputManager::InputEvent::DOWN && listSel_ + 1U < total)
        {
            ++listSel_;
        }
        else if (input == InputManager::InputEvent::BUTTON_PRESS)
        {
            state_ = UiState::PLUGIN_DETAIL;
        }
        else if (input == InputManager::InputEvent::LEFT)
        {
            state_ = UiState::MAIN_MENU;
        }
    }

    void handlePluginDetail(InputManager::InputEvent input)
    {
        if (input == InputManager::InputEvent::BUTTON_PRESS)
        {
            // Toggle enabled/disabled
            auto &pm = hackos::core::PluginManager::instance();
            const auto *info = pm.pluginAt(listSel_);
            if (info != nullptr)
            {
                pm.setEnabled(info->name, !info->enabled);
            }
        }
        else if (input == InputManager::InputEvent::LEFT)
        {
            state_ = UiState::PLUGIN_LIST;
        }
    }

    void handleReloadResult(InputManager::InputEvent input)
    {
        if (input == InputManager::InputEvent::BUTTON_PRESS ||
            input == InputManager::InputEvent::LEFT)
        {
            state_ = UiState::MAIN_MENU;
        }
    }

    // ── State ────────────────────────────────────────────────────────────
    UiState state_ = UiState::MAIN_MENU;
    size_t menuSel_ = 0U;
    size_t listSel_ = 0U;
    unsigned reloadCount_ = 0U;
};

} // namespace

AppBase *createPluginManagerApp()
{
    return new (std::nothrow) PluginManagerApp();
}
