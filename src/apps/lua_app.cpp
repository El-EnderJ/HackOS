/**
 * @file lua_app.cpp
 * @brief Lua Script Runner – browse and execute .lua scripts from SD card.
 *
 * Provides a file browser for `/ext/lua/` that lists available .lua scripts
 * and lets the user select and run them using the embedded LuaEngine.
 * Shows execution status, errors, and allows stopping running scripts.
 */

#include "apps/lua_app.h"

#include <cstdio>
#include <cstring>
#include <new>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "apps/hackos_app.h"
#include "core/event.h"
#include "core/event_system.h"
#include "core/lua_engine.h"
#include "hardware/display.h"
#include "hardware/input.h"
#include "storage/vfs.h"
#include "ui/toast_manager.h"
#include "ui/widgets.h"

static constexpr const char *TAG_LUAAPP = "LuaApp";

namespace
{

static constexpr size_t MAX_SCRIPTS = 16U;
static constexpr size_t SCRIPT_NAME_LEN = 24U;
static constexpr const char *LUA_DIR = "/ext/lua";

enum class LuaAppState : uint8_t
{
    MENU_MAIN,
    FILE_LIST,
    RUNNING,
    RESULT,
};

static constexpr size_t MAIN_MENU_COUNT = 3U;
static const char *const MAIN_MENU_LABELS[MAIN_MENU_COUNT] = {
    "Browse Scripts",
    "Reload",
    "Back",
};

class LuaApp final : public hackos::HackOSApp
{
public:
    LuaApp()
        : statusBar_(0, 0, 128, 8),
          mainMenu_(0, 20, 128, 36, 3),
          fileMenu_(0, 20, 128, 36, 3),
          state_(LuaAppState::MENU_MAIN),
          needsRedraw_(true),
          scriptCount_(0U),
          lastResult_(hackos::core::LuaResult::OK),
          scriptTaskHandle_(nullptr)
    {
        std::memset(scriptNames_, 0, sizeof(scriptNames_));
        std::memset(scriptPtrs_, 0, sizeof(scriptPtrs_));
        std::memset(scriptPaths_, 0, sizeof(scriptPaths_));
    }

protected:
    void on_alloc() override {}

    void on_start() override
    {
        statusBar_.setConnectivity(false, false);
        statusBar_.setBatteryLevel(100U);
        statusBar_.setTime(0U, 0U);
        mainMenu_.setItems(MAIN_MENU_LABELS, MAIN_MENU_COUNT);
        state_ = LuaAppState::MENU_MAIN;
        needsRedraw_ = true;
        scanScripts();
        ESP_LOGI(TAG_LUAAPP, "Lua App started (%u scripts)", static_cast<unsigned>(scriptCount_));
    }

    void on_event(Event *event) override
    {
        if (event == nullptr || event->type != EventType::EVT_INPUT)
        {
            return;
        }
        handleInput(static_cast<InputManager::InputEvent>(event->arg0));
    }

    void on_update() override
    {
        // Check if script task finished
        if (state_ == LuaAppState::RUNNING &&
            !hackos::core::LuaEngine::instance().isRunning() &&
            scriptTaskHandle_ == nullptr)
        {
            state_ = LuaAppState::RESULT;
            needsRedraw_ = true;
        }
    }

    void on_draw() override
    {
        if (!needsRedraw_)
        {
            return;
        }

        DisplayManager::instance().clear();
        statusBar_.draw();

        switch (state_)
        {
        case LuaAppState::MENU_MAIN:
            drawTitle("Lua Scripts");
            mainMenu_.draw();
            break;
        case LuaAppState::FILE_LIST:
            drawTitle("Select Script");
            if (scriptCount_ > 0U)
            {
                fileMenu_.draw();
            }
            else
            {
                DisplayManager::instance().drawText(2, 30, "No scripts found");
                DisplayManager::instance().drawText(2, 40, "Add .lua to /ext/lua/");
            }
            break;
        case LuaAppState::RUNNING:
            drawTitle("Running...");
            DisplayManager::instance().drawText(2, 30, "Script executing");
            DisplayManager::instance().drawText(2, 54, "Press to stop");
            break;
        case LuaAppState::RESULT:
            drawResultScreen();
            break;
        }

        DisplayManager::instance().present();
        needsRedraw_ = false;
    }

    void on_free() override
    {
        if (hackos::core::LuaEngine::instance().isRunning())
        {
            hackos::core::LuaEngine::instance().requestStop();
        }
        ESP_LOGI(TAG_LUAAPP, "Lua App freed");
    }

private:
    StatusBar    statusBar_;
    MenuListView mainMenu_;
    MenuListView fileMenu_;

    LuaAppState state_;
    bool        needsRedraw_;

    char scriptNames_[MAX_SCRIPTS][SCRIPT_NAME_LEN + 1U];
    const char *scriptPtrs_[MAX_SCRIPTS];
    char scriptPaths_[MAX_SCRIPTS][48];
    size_t scriptCount_;

    hackos::core::LuaResult lastResult_;
    TaskHandle_t scriptTaskHandle_;

    void drawTitle(const char *title)
    {
        DisplayManager::instance().drawText(2, 10, title);
        DisplayManager::instance().drawLine(0, 18, 127, 18);
    }

    void drawResultScreen()
    {
        drawTitle("Result");

        const char *resStr = hackos::core::luaResultStr(lastResult_);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "Status: %s", resStr);
        DisplayManager::instance().drawText(2, 24, buf);

        if (lastResult_ != hackos::core::LuaResult::OK)
        {
            auto &eng = hackos::core::LuaEngine::instance();
            std::snprintf(buf, sizeof(buf), "Line: %u",
                          static_cast<unsigned>(eng.errorLine()));
            DisplayManager::instance().drawText(2, 34, buf);

            const char *msg = eng.errorMsg();
            if (msg[0] != '\0')
            {
                // Truncate long messages
                std::snprintf(buf, sizeof(buf), "%.21s", msg);
                DisplayManager::instance().drawText(2, 44, buf);
            }
        }

        DisplayManager::instance().drawText(2, 54, "Press to go back");
    }

    void scanScripts()
    {
        scriptCount_ = 0U;
        auto &vfs = hackos::storage::VirtualFS::instance();

        hackos::storage::VirtualFS::DirEntry entries[MAX_SCRIPTS];
        size_t count = vfs.listDir(LUA_DIR, entries, MAX_SCRIPTS);

        for (size_t i = 0U; i < count && scriptCount_ < MAX_SCRIPTS; ++i)
        {
            if (entries[i].isDir)
            {
                continue;
            }
            // Check .lua extension
            const char *name = entries[i].name;
            size_t nameLen = std::strlen(name);
            if (nameLen < 5U)
            {
                continue;
            }
            if (std::strcmp(name + nameLen - 4U, ".lua") != 0)
            {
                continue;
            }

            std::strncpy(scriptNames_[scriptCount_], name, SCRIPT_NAME_LEN);
            scriptNames_[scriptCount_][SCRIPT_NAME_LEN] = '\0';
            scriptPtrs_[scriptCount_] = scriptNames_[scriptCount_];

            std::snprintf(scriptPaths_[scriptCount_], sizeof(scriptPaths_[0]),
                          "%s/%s", LUA_DIR, name);

            ++scriptCount_;
        }

        if (scriptCount_ > 0U)
        {
            fileMenu_.setItems(scriptPtrs_, scriptCount_);
        }
    }

    void handleInput(InputManager::InputEvent input)
    {
        switch (state_)
        {
        case LuaAppState::MENU_MAIN:
            handleMainInput(input);
            break;
        case LuaAppState::FILE_LIST:
            handleFileInput(input);
            break;
        case LuaAppState::RUNNING:
            if (input == InputManager::InputEvent::BUTTON_PRESS)
            {
                hackos::core::LuaEngine::instance().requestStop();
                lastResult_ = hackos::core::LuaResult::ERR_STOPPED;
                state_ = LuaAppState::RESULT;
                needsRedraw_ = true;
            }
            break;
        case LuaAppState::RESULT:
            if (input == InputManager::InputEvent::BUTTON_PRESS)
            {
                state_ = LuaAppState::MENU_MAIN;
                needsRedraw_ = true;
            }
            break;
        }
    }

    void handleMainInput(InputManager::InputEvent input)
    {
        if (input == InputManager::InputEvent::UP)
        {
            mainMenu_.moveSelection(-1);
            needsRedraw_ = true;
        }
        else if (input == InputManager::InputEvent::DOWN)
        {
            mainMenu_.moveSelection(1);
            needsRedraw_ = true;
        }
        else if (input == InputManager::InputEvent::BUTTON_PRESS)
        {
            switch (mainMenu_.selectedIndex())
            {
            case 0U: // Browse
                state_ = LuaAppState::FILE_LIST;
                needsRedraw_ = true;
                break;
            case 1U: // Reload
                scanScripts();
                ToastManager::instance().show("Scripts reloaded");
                needsRedraw_ = true;
                break;
            case 2U: // Back
            {
                const Event evt{EventType::EVT_SYSTEM, SYSTEM_EVENT_BACK, 0, nullptr};
                EventSystem::instance().postEvent(evt);
                break;
            }
            default:
                break;
            }
        }
    }

    void handleFileInput(InputManager::InputEvent input)
    {
        if (input == InputManager::InputEvent::UP)
        {
            fileMenu_.moveSelection(-1);
            needsRedraw_ = true;
        }
        else if (input == InputManager::InputEvent::DOWN)
        {
            fileMenu_.moveSelection(1);
            needsRedraw_ = true;
        }
        else if (input == InputManager::InputEvent::LEFT)
        {
            state_ = LuaAppState::MENU_MAIN;
            needsRedraw_ = true;
        }
        else if (input == InputManager::InputEvent::BUTTON_PRESS)
        {
            if (scriptCount_ > 0U)
            {
                runScript(fileMenu_.selectedIndex());
            }
        }
    }

    void runScript(size_t idx)
    {
        if (idx >= scriptCount_)
        {
            return;
        }

        auto &eng = hackos::core::LuaEngine::instance();

        lastResult_ = eng.loadFile(scriptPaths_[idx]);
        if (lastResult_ != hackos::core::LuaResult::OK)
        {
            state_ = LuaAppState::RESULT;
            needsRedraw_ = true;
            return;
        }

        state_ = LuaAppState::RUNNING;
        needsRedraw_ = true;

        // Execute synchronously (cooperative via vTaskDelay in engine)
        lastResult_ = eng.execute();

        state_ = LuaAppState::RESULT;
        scriptTaskHandle_ = nullptr;
        needsRedraw_ = true;

        if (lastResult_ == hackos::core::LuaResult::OK)
        {
            ToastManager::instance().show("Script completed!");
            EventSystem::instance().postEvent(
                {EventType::EVT_XP_EARNED, XP_PLUGIN_LOAD, 0, nullptr});
        }
        else
        {
            char msg[40];
            std::snprintf(msg, sizeof(msg), "Error L%u: %s",
                          static_cast<unsigned>(eng.errorLine()),
                          hackos::core::luaResultStr(lastResult_));
            ToastManager::instance().show(msg);
        }
    }
};

} // namespace

AppBase *createLuaApp()
{
    return new (std::nothrow) LuaApp();
}
