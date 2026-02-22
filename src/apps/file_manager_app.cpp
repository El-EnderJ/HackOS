#include "apps/file_manager_app.h"

#include <cstdio>
#include <cstring>
#include <esp_log.h>
#include <new>

#include "core/event.h"
#include "core/event_system.h"
#include "hardware/display.h"
#include "hardware/input.h"
#include "hardware/storage.h"
#include "ui/widgets.h"

static constexpr const char *TAG_FM = "FileManagerApp";

namespace
{

// ── Internal state machine ────────────────────────────────────────────────────

enum class FMState : uint8_t
{
    BROWSING,
    NO_SD,
};

// ── App class ─────────────────────────────────────────────────────────────────

class FileManagerApp final : public AppBase, public IEventObserver
{
public:
    FileManagerApp()
        : statusBar_(0, 0, 128, 8),
          menu_(0, 20, 128, 36, 3),
          state_(FMState::BROWSING),
          needsRedraw_(true),
          entryCount_(0U),
          entries_{},
          entryLabelPtrs_{}
    {
        currentPath_[0] = '\0';
        for (size_t i = 0U; i < MAX_ENTRIES; ++i)
        {
            entryLabels_[i][0] = '\0';
        }
    }

    void onSetup() override
    {
        statusBar_.setBatteryLevel(100U);
        statusBar_.setTime(0U, 0U);
        (void)EventSystem::instance().subscribe(this);
        std::strncpy(currentPath_, "/", sizeof(currentPath_) - 1U);
        currentPath_[sizeof(currentPath_) - 1U] = '\0';
        if (!StorageManager::instance().isMounted())
        {
            state_ = FMState::NO_SD;
            statusBar_.setConnectivity(false, false);
        }
        else
        {
            state_ = FMState::BROWSING;
            statusBar_.setConnectivity(true, false);
            refreshDir();
        }
        needsRedraw_ = true;
        ESP_LOGI(TAG_FM, "setup, path=%s", currentPath_);
    }

    void onLoop() override {}

    void onDraw() override
    {
        if (!needsRedraw_ && !statusBar_.isDirty() && !menu_.isDirty())
        {
            return;
        }

        DisplayManager::instance().clear();
        statusBar_.draw();

        switch (state_)
        {
        case FMState::BROWSING:
            drawTitle("File Manager");
            if (entryCount_ > 0U)
            {
                menu_.draw();
            }
            else
            {
                DisplayManager::instance().drawText(4, 28, "(empty)");
            }
            drawPathHint();
            break;

        case FMState::NO_SD:
            drawTitle("File Manager");
            DisplayManager::instance().drawText(4, 28, "SD not mounted");
            DisplayManager::instance().drawText(4, 40, "Insert SD card");
            break;
        }

        DisplayManager::instance().present();
        statusBar_.clearDirty();
        menu_.clearDirty();
        needsRedraw_ = false;
    }

    void onEvent(Event *event) override
    {
        if (event == nullptr || event->type != EventType::EVT_INPUT)
        {
            return;
        }
        handleInput(static_cast<InputManager::InputEvent>(event->arg0));
    }

    void onDestroy() override
    {
        EventSystem::instance().unsubscribe(this);
        ESP_LOGI(TAG_FM, "destroyed");
    }

private:
    static constexpr size_t MAX_ENTRIES = 16U;
    static constexpr size_t LABEL_LEN = 72U;   // 64 name + "/" + size digits + NUL
    static constexpr size_t PATH_LEN = 128U;

    StatusBar statusBar_;
    MenuListView menu_;
    FMState state_;
    bool needsRedraw_;
    size_t entryCount_;
    StorageManager::DirEntry entries_[MAX_ENTRIES];
    char entryLabels_[MAX_ENTRIES][LABEL_LEN];
    const char *entryLabelPtrs_[MAX_ENTRIES];
    char currentPath_[PATH_LEN];

    // ── Drawing helpers ───────────────────────────────────────────────────────

    void drawTitle(const char *title)
    {
        DisplayManager::instance().drawText(2, 10, title);
        DisplayManager::instance().drawLine(0, 18, 127, 18);
    }

    void drawPathHint()
    {
        // Truncate current path from the right so it fits in ~21 chars on a 128px display
        char truncated[22];
        const size_t pathLen = std::strlen(currentPath_);
        if (pathLen <= 21U)
        {
            std::snprintf(truncated, sizeof(truncated), "%s", currentPath_);
        }
        else
        {
            std::snprintf(truncated, sizeof(truncated), "...%.18s",
                          currentPath_ + pathLen - 18U);
        }
        DisplayManager::instance().drawText(2, 56, truncated);
    }

    // ── Directory management ──────────────────────────────────────────────────

    void refreshDir()
    {
        entryCount_ = StorageManager::instance().listDir(currentPath_, entries_, MAX_ENTRIES);
        buildLabels();
        menu_.setItems(entryLabelPtrs_, entryCount_);
        ESP_LOGI(TAG_FM, "listed %s: %u entries", currentPath_,
                 static_cast<unsigned>(entryCount_));
    }

    void buildLabels()
    {
        for (size_t i = 0U; i < entryCount_; ++i)
        {
            if (entries_[i].isDir)
            {
                std::snprintf(entryLabels_[i], LABEL_LEN, "[%s]", entries_[i].name);
            }
            else if (entries_[i].size >= 1024U)
            {
                std::snprintf(entryLabels_[i], LABEL_LEN, "%-16s%luK",
                              entries_[i].name,
                              static_cast<unsigned long>(entries_[i].size / 1024U));
            }
            else
            {
                std::snprintf(entryLabels_[i], LABEL_LEN, "%-16s%luB",
                              entries_[i].name,
                              static_cast<unsigned long>(entries_[i].size));
            }
            entryLabelPtrs_[i] = entryLabels_[i];
        }
    }

    void enterSelected()
    {
        if (entryCount_ == 0U)
        {
            return;
        }
        const size_t sel = menu_.selectedIndex();
        if (sel >= entryCount_)
        {
            return;
        }
        if (!entries_[sel].isDir)
        {
            // File selected – show size hint and stay (no viewer needed)
            needsRedraw_ = true;
            return;
        }

        // Build child path – validate it fits before committing
        char child[PATH_LEN];
        const size_t curLen = std::strlen(currentPath_);
        const bool trailingSlash = (curLen > 0U && currentPath_[curLen - 1U] == '/');
        const int written = std::snprintf(child, sizeof(child), trailingSlash ? "%s%s" : "%s/%s",
                                          currentPath_, entries_[sel].name);
        if (written <= 0 || static_cast<size_t>(written) >= sizeof(child))
        {
            ESP_LOGW(TAG_FM, "enterSelected: path too long, ignoring");
            return;
        }
        std::strncpy(currentPath_, child, sizeof(currentPath_) - 1U);
        currentPath_[sizeof(currentPath_) - 1U] = '\0';
        refreshDir();
        needsRedraw_ = true;
    }

    void goUp()
    {
        // If already at root, exit to launcher
        if (std::strcmp(currentPath_, "/") == 0)
        {
            const Event evt{EventType::EVT_SYSTEM, SYSTEM_EVENT_BACK, 0, nullptr};
            EventSystem::instance().postEvent(evt);
            return;
        }

        // Strip the last path component
        char *lastSlash = nullptr;
        char *p = currentPath_;
        while (*p != '\0')
        {
            if (*p == '/')
            {
                lastSlash = p;
            }
            ++p;
        }
        if (lastSlash != nullptr)
        {
            if (lastSlash == currentPath_)
            {
                // Parent is root
                currentPath_[1] = '\0';
            }
            else
            {
                *lastSlash = '\0';
            }
        }
        refreshDir();
        needsRedraw_ = true;
    }

    // ── Input handling ────────────────────────────────────────────────────────

    void handleInput(InputManager::InputEvent input)
    {
        if (state_ == FMState::NO_SD)
        {
            if (input == InputManager::InputEvent::BUTTON_PRESS ||
                input == InputManager::InputEvent::LEFT)
            {
                const Event evt{EventType::EVT_SYSTEM, SYSTEM_EVENT_BACK, 0, nullptr};
                EventSystem::instance().postEvent(evt);
            }
            return;
        }

        // BROWSING state
        if (input == InputManager::InputEvent::UP)
        {
            menu_.moveSelection(-1);
        }
        else if (input == InputManager::InputEvent::DOWN)
        {
            menu_.moveSelection(1);
        }
        else if (input == InputManager::InputEvent::BUTTON_PRESS)
        {
            enterSelected();
        }
        else if (input == InputManager::InputEvent::LEFT)
        {
            goUp();
        }
    }
};

} // namespace

AppBase *createFileManagerApp()
{
    return new (std::nothrow) FileManagerApp();
}
