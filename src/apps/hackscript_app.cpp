/**
 * @file hackscript_app.cpp
 * @brief HackScript macro runner UI – browse, load and execute .hs scripts.
 *
 * Lists `.hs` files found in `/ext/scripts/`, lets the user select one,
 * and runs it through the HackScriptEngine with a live progress view.
 */

#include "apps/hackscript_app.h"

#include <cstdio>
#include <cstring>
#include <new>

#include <esp_log.h>

#include "core/event.h"
#include "core/event_system.h"
#include "core/hackscript_engine.h"
#include "hardware/display.h"
#include "hardware/input.h"
#include "storage/vfs.h"

static constexpr const char *TAG_HSA = "HackScriptApp";
static constexpr const char *SCRIPTS_DIR = "/ext/scripts";

namespace
{

static constexpr size_t MAX_SCRIPTS = 16U;

enum class HSView : uint8_t
{
    FILE_LIST,
    RUNNING,
};

class HackScriptAppImpl final : public AppBase
{
public:
    HackScriptAppImpl()
        : view_(HSView::FILE_LIST),
          fileCount_(0U),
          sel_(0U)
    {
        std::memset(files_, 0, sizeof(files_));
    }

    void onSetup() override
    {
        scanScripts();
    }

    void onLoop() override
    {
        if (view_ == HSView::RUNNING)
        {
            auto &engine = hackos::core::HackScriptEngine::instance();
            if (!engine.tick())
            {
                view_ = HSView::FILE_LIST;
            }
        }
    }

    void onDraw() override
    {
        auto &d = DisplayManager::instance();
        d.clear();

        if (view_ == HSView::FILE_LIST)
        {
            d.drawText(0, 0, "HackScript", 1U);
            d.drawLine(0, 10, 127, 10);

            if (fileCount_ == 0U)
            {
                d.drawText(0, 20, "No .hs files in");
                d.drawText(0, 30, SCRIPTS_DIR);
            }
            else
            {
                for (size_t i = 0U; i < fileCount_ && i < 5U; ++i)
                {
                    const size_t idx = (sel_ < 5U) ? i : (sel_ - 4U + i);
                    if (idx >= fileCount_)
                    {
                        break;
                    }
                    const int16_t y = static_cast<int16_t>(12 + i * 10);
                    if (idx == sel_)
                    {
                        d.fillRect(0, y, 128, 10, SSD1306_WHITE);
                        d.drawText(2, y + 1, files_[idx].name, 1U, SSD1306_BLACK);
                    }
                    else
                    {
                        d.drawText(2, y + 1, files_[idx].name, 1U, SSD1306_WHITE);
                    }
                }
            }
        }
        else
        {
            auto &engine = hackos::core::HackScriptEngine::instance();
            d.drawText(0, 0, "Running script...", 1U);
            d.drawLine(0, 10, 127, 10);

            char lineBuf[32];
            std::snprintf(lineBuf, sizeof(lineBuf), "Line %u / %u",
                          static_cast<unsigned>(engine.currentLine() + 1U),
                          static_cast<unsigned>(engine.totalLines()));
            d.drawText(0, 14, lineBuf);

            const char *cmd = engine.currentCommand();
            if (cmd != nullptr && cmd[0] != '\0')
            {
                // Truncate to fit 128px wide OLED (21 chars at 6px each).
                static constexpr size_t DISPLAY_MAX_CHARS = 21U;
                char cmdDisp[DISPLAY_MAX_CHARS + 1U];
                std::strncpy(cmdDisp, cmd, DISPLAY_MAX_CHARS);
                cmdDisp[DISPLAY_MAX_CHARS] = '\0';
                d.drawText(0, 28, cmdDisp);
            }

            // Progress bar
            const uint8_t pct = (engine.totalLines() > 0U)
                                    ? static_cast<uint8_t>((engine.currentLine() * 100U) / engine.totalLines())
                                    : 0U;
            d.drawRect(0, 50, 128, 10);
            const int16_t barW = static_cast<int16_t>((pct * 124) / 100);
            d.fillRect(2, 52, barW, 6);
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

        if (view_ == HSView::RUNNING)
        {
            if (input == InputManager::InputEvent::LEFT)
            {
                hackos::core::HackScriptEngine::instance().stop();
                view_ = HSView::FILE_LIST;
            }
            return;
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
            if (sel_ + 1U < fileCount_)
            {
                ++sel_;
            }
            break;
        case InputManager::InputEvent::BUTTON_PRESS:
            if (fileCount_ > 0U)
            {
                char fullPath[96];
                std::snprintf(fullPath, sizeof(fullPath), "%s/%s", SCRIPTS_DIR, files_[sel_].name);
                if (hackos::core::HackScriptEngine::instance().load(fullPath))
                {
                    view_ = HSView::RUNNING;
                }
            }
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

    void onDestroy() override
    {
        hackos::core::HackScriptEngine::instance().stop();
    }

private:
    void scanScripts()
    {
        fileCount_ = 0U;
        auto &vfs = hackos::storage::VirtualFS::instance();
        hackos::storage::VirtualFS::DirEntry entries[MAX_SCRIPTS];
        const size_t count = vfs.listDir(SCRIPTS_DIR, entries, MAX_SCRIPTS);

        for (size_t i = 0U; i < count && fileCount_ < MAX_SCRIPTS; ++i)
        {
            if (entries[i].isDir)
            {
                continue;
            }
            // Check for .hs extension
            const size_t nameLen = std::strlen(entries[i].name);
            if (nameLen > 3U &&
                std::strcmp(entries[i].name + nameLen - 3U, ".hs") == 0)
            {
                files_[fileCount_] = entries[i];
                ++fileCount_;
            }
        }
        sel_ = 0U;
        ESP_LOGI(TAG_HSA, "Found %u scripts", static_cast<unsigned>(fileCount_));
    }

    HSView view_;
    hackos::storage::VirtualFS::DirEntry files_[MAX_SCRIPTS];
    size_t fileCount_;
    size_t sel_;
};

} // anonymous namespace

AppBase *createHackScriptApp()
{
    return new (std::nothrow) HackScriptAppImpl();
}
