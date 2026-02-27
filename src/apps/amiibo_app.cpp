/**
 * @file amiibo_app.cpp
 * @brief Phase 12 – Amiibo Master & NFC Emulator (NTAG215).
 *
 * Features:
 *  - **SD Amiibo Browser**: Navigate /ext/nfc/amiibo/ folders and select
 *    .bin files (540 bytes) for emulation.
 *  - **NTAG215 Emulation**: PN532 tgInitAsTarget serving page reads from
 *    a loaded .bin dump, including PWD_AUTH handling.
 *  - **Write to Tag**: Write the loaded .bin to a blank NTAG215 tag.
 *  - **Amiibo Keys**: Generate a placeholder amiibo_keys.bin on SD.
 */

#include "apps/amiibo_app.h"

#include <cstdio>
#include <cstring>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <new>

#include "core/event.h"
#include "core/event_system.h"
#include "hardware/display.h"
#include "hardware/input.h"
#include "hardware/nfc_reader.h"
#include "hardware/storage.h"
#include "ui/widgets.h"

static constexpr const char *TAG_AMIIBO = "AmiiboApp";

namespace
{

// ── App states ──────────────────────────────────────────────────────────────

enum class AmiiboState : uint8_t
{
    MAIN_MENU,
    BROWSING,
    FILE_SELECTED,
    EMULATING,
    WRITING,
    GENERATE_KEYS,
};

// ── Menu labels ─────────────────────────────────────────────────────────────

static constexpr size_t MAIN_MENU_COUNT = 4U;
static const char *const MAIN_MENU_LABELS[MAIN_MENU_COUNT] = {
    "Browse Amiibo",
    "Generate Keys",
    "Write to Tag",
    "Back",
};

// ── Constants ───────────────────────────────────────────────────────────────

/// Root path for Amiibo binary files on SD.
static constexpr const char *AMIIBO_ROOT = "/ext/nfc/amiibo";

/// Maximum directory entries to display.
static constexpr size_t MAX_DIR_ENTRIES = 16U;

/// Maximum path length.
static constexpr size_t MAX_PATH_LEN = 128U;

/// Maximum label length for directory entries.
static constexpr size_t LABEL_LEN = 24U;

/// NTAG215 dump size in bytes.
static constexpr size_t NTAG215_SIZE = NFCReader::NTAG215_SIZE;

/// Amiibo keys file path.
static constexpr const char *AMIIBO_KEYS_PATH = "/ext/nfc/amiibo/amiibo_keys.bin";

/// Placeholder amiibo key data (80 bytes – typical key file structure).
static constexpr size_t AMIIBO_KEYS_SIZE = 80U;

// ── App class ───────────────────────────────────────────────────────────────

class AmiiboApp final : public AppBase, public IEventObserver
{
public:
    AmiiboApp()
        : statusBar_(0, 0, 128, 8),
          mainMenu_(0, 20, 128, 36, 3),
          fileMenu_(0, 20, 128, 36, 3),
          state_(AmiiboState::MAIN_MENU),
          needsRedraw_(true),
          dumpLoaded_(false),
          amiiboName_{},
          currentPath_{},
          statusLine_{},
          entryCount_(0U),
          dumpBuf_(nullptr)
    {
    }

    void onSetup() override
    {
        statusBar_.setConnectivity(false, false);
        statusBar_.setBatteryLevel(100U);
        statusBar_.setTime(0U, 0U);
        mainMenu_.setItems(MAIN_MENU_LABELS, MAIN_MENU_COUNT);
        (void)EventSystem::instance().subscribe(this);
        state_ = AmiiboState::MAIN_MENU;
        needsRedraw_ = true;

        // Allocate dump buffer on heap (540 bytes, single contiguous block)
        dumpBuf_ = static_cast<uint8_t *>(heap_caps_malloc(NTAG215_SIZE, MALLOC_CAP_8BIT));
        if (dumpBuf_ != nullptr)
        {
            std::memset(dumpBuf_, 0, NTAG215_SIZE);
        }
        else
        {
            ESP_LOGE(TAG_AMIIBO, "Failed to allocate dump buffer");
        }

        ESP_LOGI(TAG_AMIIBO, "setup");
    }

    void onLoop() override
    {
        // No polling-based work needed; all transitions are event-driven.
    }

    void onDraw() override
    {
        if (!needsRedraw_ && !statusBar_.isDirty() &&
            !mainMenu_.isDirty() && !fileMenu_.isDirty())
        {
            return;
        }

        DisplayManager::instance().clear();
        statusBar_.draw();

        switch (state_)
        {
        case AmiiboState::MAIN_MENU:
            drawTitle("Amiibo Master");
            mainMenu_.draw();
            break;
        case AmiiboState::BROWSING:
            drawTitle("Amiibo Files");
            fileMenu_.draw();
            break;
        case AmiiboState::FILE_SELECTED:
            drawTitle("Amiibo Loaded");
            drawFileInfo();
            break;
        case AmiiboState::EMULATING:
            drawTitle("Emulating...");
            drawEmulatingView();
            break;
        case AmiiboState::WRITING:
            drawTitle("Write to Tag");
            drawStatus();
            break;
        case AmiiboState::GENERATE_KEYS:
            drawTitle("Keys");
            drawStatus();
            break;
        }

        DisplayManager::instance().present();
        statusBar_.clearDirty();
        mainMenu_.clearDirty();
        fileMenu_.clearDirty();
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
        if (dumpBuf_ != nullptr)
        {
            heap_caps_free(dumpBuf_);
            dumpBuf_ = nullptr;
        }
        NFCReader::instance().deinit();
        EventSystem::instance().unsubscribe(this);
        ESP_LOGI(TAG_AMIIBO, "destroyed");
    }

private:
    static constexpr size_t STATUS_LEN = 32U;

    StatusBar statusBar_;
    MenuListView mainMenu_;
    MenuListView fileMenu_;
    AmiiboState state_;
    bool needsRedraw_;
    bool dumpLoaded_;
    char amiiboName_[32];
    char currentPath_[MAX_PATH_LEN];
    char statusLine_[STATUS_LEN];
    size_t entryCount_;
    uint8_t *dumpBuf_;

    /// Cached directory entries.
    StorageManager::DirEntry entries_[MAX_DIR_ENTRIES];
    /// Menu labels for file browser (heap-free: stored inline).
    char entryLabels_[MAX_DIR_ENTRIES][LABEL_LEN];
    /// Pointers to labels for MenuListView.
    const char *labelPtrs_[MAX_DIR_ENTRIES];

    // ── Helpers ─────────────────────────────────────────────────────────────

    void transitionTo(AmiiboState next)
    {
        state_ = next;
        needsRedraw_ = true;
    }

    bool ensureNfcReady()
    {
        if (!NFCReader::instance().isReady())
        {
            if (!NFCReader::instance().init())
            {
                std::snprintf(statusLine_, sizeof(statusLine_), "PN532 SPI error");
                needsRedraw_ = true;
                ESP_LOGE(TAG_AMIIBO, "PN532 init failed");
                return false;
            }
        }
        return true;
    }

    // ── Drawing ─────────────────────────────────────────────────────────────

    void drawTitle(const char *title)
    {
        DisplayManager::instance().drawText(2, 10, title);
        DisplayManager::instance().drawLine(0, 18, 127, 18);
    }

    void drawStatus()
    {
        DisplayManager::instance().drawText(2, 32, statusLine_);
    }

    void drawFileInfo()
    {
        char line[32];
        std::snprintf(line, sizeof(line), "Name: %.20s", amiiboName_);
        DisplayManager::instance().drawText(2, 24, line);
        DisplayManager::instance().drawText(2, 36, "540 bytes NTAG215");
        DisplayManager::instance().drawText(2, 48, "OK:Emulate  DN:Write");
        DisplayManager::instance().drawText(2, 58, "LEFT:Back");
    }

    void drawEmulatingView()
    {
        char line[32];
        std::snprintf(line, sizeof(line), "%.20s", amiiboName_);
        DisplayManager::instance().drawText(2, 24, line);

        // Simple emulation icon: NFC waves
        DisplayManager::instance().drawText(2, 38, "((( NFC )))");
        DisplayManager::instance().drawText(2, 50, "Emulating...");
        DisplayManager::instance().drawText(2, 58, "Press to stop");
    }

    // ── SD browser ──────────────────────────────────────────────────────────

    void openBrowser()
    {
        if (!StorageManager::instance().isMounted())
        {
            std::snprintf(statusLine_, sizeof(statusLine_), "SD not mounted");
            needsRedraw_ = true;
            return;
        }

        std::strncpy(currentPath_, AMIIBO_ROOT, sizeof(currentPath_) - 1U);
        currentPath_[sizeof(currentPath_) - 1U] = '\0';
        loadDirectory();
        transitionTo(AmiiboState::BROWSING);
    }

    void loadDirectory()
    {
        entryCount_ = StorageManager::instance().listDir(
            currentPath_, entries_, MAX_DIR_ENTRIES);

        for (size_t i = 0U; i < entryCount_; ++i)
        {
            if (entries_[i].isDir)
            {
                std::snprintf(entryLabels_[i], LABEL_LEN, "[%s]", entries_[i].name);
            }
            else
            {
                const uint32_t sz = entries_[i].size;
                if (sz >= 1024U)
                {
                    std::snprintf(entryLabels_[i], LABEL_LEN, "%.16s %luK",
                                  entries_[i].name,
                                  static_cast<unsigned long>(sz / 1024U));
                }
                else
                {
                    std::snprintf(entryLabels_[i], LABEL_LEN, "%.16s %luB",
                                  entries_[i].name,
                                  static_cast<unsigned long>(sz));
                }
            }
            labelPtrs_[i] = entryLabels_[i];
        }

        if (entryCount_ == 0U)
        {
            entryCount_ = 1U;
            std::strncpy(entryLabels_[0], "(empty)", LABEL_LEN);
            labelPtrs_[0] = entryLabels_[0];
        }

        fileMenu_.setItems(labelPtrs_, entryCount_);
        needsRedraw_ = true;
    }

    void selectEntry(size_t idx)
    {
        if (idx >= entryCount_)
        {
            return;
        }

        // Check for empty placeholder
        if (std::strcmp(entryLabels_[0], "(empty)") == 0)
        {
            return;
        }

        if (entries_[idx].isDir)
        {
            // Descend into directory
            const size_t curLen = std::strlen(currentPath_);
            const size_t nameLen = std::strlen(entries_[idx].name);
            if (curLen + 1U + nameLen < MAX_PATH_LEN)
            {
                currentPath_[curLen] = '/';
                std::memcpy(currentPath_ + curLen + 1U, entries_[idx].name, nameLen + 1U);
                loadDirectory();
            }
        }
        else
        {
            // Check if it's a 540-byte .bin file
            if (entries_[idx].size != NTAG215_SIZE)
            {
                std::snprintf(statusLine_, sizeof(statusLine_), "Not 540B NTAG215");
                needsRedraw_ = true;
                return;
            }

            // Load the .bin file into the dump buffer
            if (loadBinFile(idx))
            {
                transitionTo(AmiiboState::FILE_SELECTED);
            }
        }
    }

    bool loadBinFile(size_t idx)
    {
        if (dumpBuf_ == nullptr)
        {
            std::snprintf(statusLine_, sizeof(statusLine_), "No RAM buffer");
            needsRedraw_ = true;
            return false;
        }

        // Build full path
        char fullPath[MAX_PATH_LEN];
        std::snprintf(fullPath, sizeof(fullPath), "%s/%s",
                      currentPath_, entries_[idx].name);

        // Read the file using StorageManager
        size_t bytesRead = 0U;
        if (!StorageManager::instance().readFile(fullPath, dumpBuf_, NTAG215_SIZE, &bytesRead))
        {
            std::snprintf(statusLine_, sizeof(statusLine_), "Read failed");
            needsRedraw_ = true;
            ESP_LOGE(TAG_AMIIBO, "Cannot read %s", fullPath);
            dumpLoaded_ = false;
            return false;
        }

        if (bytesRead != NTAG215_SIZE)
        {
            std::snprintf(statusLine_, sizeof(statusLine_), "Read err: %u B",
                          static_cast<unsigned>(bytesRead));
            needsRedraw_ = true;
            dumpLoaded_ = false;
            return false;
        }

        dumpLoaded_ = true;

        // Extract name from filename (strip .bin extension)
        std::strncpy(amiiboName_, entries_[idx].name, sizeof(amiiboName_) - 1U);
        amiiboName_[sizeof(amiiboName_) - 1U] = '\0';
        char *dot = std::strrchr(amiiboName_, '.');
        if (dot != nullptr)
        {
            *dot = '\0';
        }

        ESP_LOGI(TAG_AMIIBO, "Loaded: %s (%u bytes)", amiiboName_,
                 static_cast<unsigned>(bytesRead));
        return true;
    }

    void navigateUp()
    {
        // Go up one directory level
        char *lastSlash = std::strrchr(currentPath_, '/');
        if (lastSlash != nullptr && lastSlash != currentPath_)
        {
            // Don't go above the amiibo root
            if (std::strlen(currentPath_) <= std::strlen(AMIIBO_ROOT))
            {
                transitionTo(AmiiboState::MAIN_MENU);
                mainMenu_.setItems(MAIN_MENU_LABELS, MAIN_MENU_COUNT);
                return;
            }
            *lastSlash = '\0';
            loadDirectory();
        }
        else
        {
            transitionTo(AmiiboState::MAIN_MENU);
            mainMenu_.setItems(MAIN_MENU_LABELS, MAIN_MENU_COUNT);
        }
    }

    // ── Emulation ───────────────────────────────────────────────────────────

    void startEmulation()
    {
        if (!dumpLoaded_ || dumpBuf_ == nullptr)
        {
            std::snprintf(statusLine_, sizeof(statusLine_), "No Amiibo loaded");
            needsRedraw_ = true;
            return;
        }

        if (!ensureNfcReady())
        {
            return;
        }

        std::snprintf(statusLine_, sizeof(statusLine_), "Emulating...");
        transitionTo(AmiiboState::EMULATING);

        const bool ok = NFCReader::instance().emulateNtag215(dumpBuf_, 30000U);

        std::snprintf(statusLine_, sizeof(statusLine_),
                      ok ? "Emulation OK" : "Timeout/no reader");
        needsRedraw_ = true;
    }

    // ── Write to Tag ────────────────────────────────────────────────────────

    void startWriteToTag()
    {
        if (!dumpLoaded_ || dumpBuf_ == nullptr)
        {
            std::snprintf(statusLine_, sizeof(statusLine_), "No Amiibo loaded");
            transitionTo(AmiiboState::WRITING);
            return;
        }

        if (!ensureNfcReady())
        {
            transitionTo(AmiiboState::WRITING);
            return;
        }

        std::snprintf(statusLine_, sizeof(statusLine_), "Place NTAG215...");
        transitionTo(AmiiboState::WRITING);

        const bool ok = NFCReader::instance().writeNtag215(dumpBuf_);

        std::snprintf(statusLine_, sizeof(statusLine_),
                      ok ? "Write complete!" : "Write failed");
        needsRedraw_ = true;
    }

    // ── Key generation ──────────────────────────────────────────────────────

    void generateKeys()
    {
        if (!StorageManager::instance().isMounted())
        {
            std::snprintf(statusLine_, sizeof(statusLine_), "SD not mounted");
            transitionTo(AmiiboState::GENERATE_KEYS);
            return;
        }

        // Generate a placeholder amiibo_keys.bin
        // Real keys would come from a legitimate source; this creates
        // a zeroed placeholder file of the expected size.
        uint8_t keyBuf[AMIIBO_KEYS_SIZE];
        std::memset(keyBuf, 0, sizeof(keyBuf));

        // Write a header marker so the file is identifiable
        keyBuf[0] = 0xAAU;
        keyBuf[1] = 0xBBU;
        keyBuf[2] = 0xCCU;
        keyBuf[3] = 0xDDU;

        const bool ok = StorageManager::instance().writeFile(
            "/nfc/amiibo/amiibo_keys.bin", keyBuf, sizeof(keyBuf));

        std::snprintf(statusLine_, sizeof(statusLine_),
                      ok ? "Keys saved to SD" : "Write failed");
        transitionTo(AmiiboState::GENERATE_KEYS);
        ESP_LOGI(TAG_AMIIBO, "generateKeys: %s", ok ? "OK" : "FAIL");
    }

    // ── Input handling ──────────────────────────────────────────────────────

    void handleInput(InputManager::InputEvent input)
    {
        switch (state_)
        {
        case AmiiboState::MAIN_MENU:
            handleMainMenu(input);
            break;
        case AmiiboState::BROWSING:
            handleBrowsing(input);
            break;
        case AmiiboState::FILE_SELECTED:
            handleFileSelected(input);
            break;
        case AmiiboState::EMULATING:
            if (input == InputManager::InputEvent::BUTTON_PRESS ||
                input == InputManager::InputEvent::LEFT)
            {
                transitionTo(AmiiboState::FILE_SELECTED);
            }
            break;
        case AmiiboState::WRITING:
        case AmiiboState::GENERATE_KEYS:
            if (input == InputManager::InputEvent::BUTTON_PRESS ||
                input == InputManager::InputEvent::LEFT)
            {
                transitionTo(AmiiboState::MAIN_MENU);
                mainMenu_.setItems(MAIN_MENU_LABELS, MAIN_MENU_COUNT);
            }
            break;
        }
    }

    void handleMainMenu(InputManager::InputEvent input)
    {
        if (input == InputManager::InputEvent::UP)
        {
            mainMenu_.moveSelection(-1);
        }
        else if (input == InputManager::InputEvent::DOWN)
        {
            mainMenu_.moveSelection(1);
        }
        else if (input == InputManager::InputEvent::BUTTON_PRESS)
        {
            const size_t sel = mainMenu_.selectedIndex();
            if (sel == 0U) // Browse Amiibo
            {
                openBrowser();
            }
            else if (sel == 1U) // Generate Keys
            {
                generateKeys();
            }
            else if (sel == 2U) // Write to Tag
            {
                startWriteToTag();
            }
            else // Back
            {
                const Event evt{EventType::EVT_SYSTEM, SYSTEM_EVENT_BACK, 0, nullptr};
                EventSystem::instance().postEvent(evt);
            }
        }
    }

    void handleBrowsing(InputManager::InputEvent input)
    {
        if (input == InputManager::InputEvent::UP)
        {
            fileMenu_.moveSelection(-1);
        }
        else if (input == InputManager::InputEvent::DOWN)
        {
            fileMenu_.moveSelection(1);
        }
        else if (input == InputManager::InputEvent::BUTTON_PRESS)
        {
            selectEntry(fileMenu_.selectedIndex());
        }
        else if (input == InputManager::InputEvent::LEFT)
        {
            navigateUp();
        }
    }

    void handleFileSelected(InputManager::InputEvent input)
    {
        if (input == InputManager::InputEvent::BUTTON_PRESS)
        {
            startEmulation();
        }
        else if (input == InputManager::InputEvent::DOWN)
        {
            startWriteToTag();
        }
        else if (input == InputManager::InputEvent::LEFT)
        {
            transitionTo(AmiiboState::BROWSING);
            loadDirectory();
        }
    }
};

} // namespace

AppBase *createAmiiboApp()
{
    return new (std::nothrow) AmiiboApp();
}
