#include "apps/nfc_tools_app.h"

#include <cstdio>
#include <cstring>
#include <esp_log.h>
#include <new>

#include "core/event.h"
#include "core/event_system.h"
#include "hardware/display.h"
#include "hardware/input.h"
#include "hardware/nfc_reader.h"
#include "ui/widgets.h"

static constexpr const char *TAG_NFC_APP = "NFCToolsApp";

namespace
{

enum class NFCState : uint8_t
{
    MAIN_MENU,
    READING_UID,
    DUMPING,
    DUMP_DONE,
};

static constexpr size_t NFC_MENU_COUNT = 3U;
static const char *const NFC_MENU_LABELS[NFC_MENU_COUNT] = {"Read UID", "Dump Mifare", "Back"};

class NFCToolsApp final : public AppBase, public IEventObserver
{
public:
    NFCToolsApp()
        : statusBar_(0, 0, 128, 8),
          mainMenu_(0, 20, 128, 36, 3),
          progressBar_(0, 54, 128, 10),
          state_(NFCState::MAIN_MENU),
          needsRedraw_(true),
          uid_{},
          uidLen_(0U),
          dumpSector_(0U),
          dumpSuccess_(0U),
          dumpFail_(0U),
          statusLine_{}
    {
    }

    void onSetup() override
    {
        statusBar_.setConnectivity(false, false);
        statusBar_.setBatteryLevel(100U);
        statusBar_.setTime(0U, 0U);
        mainMenu_.setItems(NFC_MENU_LABELS, NFC_MENU_COUNT);
        progressBar_.setProgress(0U);
        (void)EventSystem::instance().subscribe(this);
        state_ = NFCState::MAIN_MENU;
        needsRedraw_ = true;
        ESP_LOGI(TAG_NFC_APP, "setup");
    }

    void onLoop() override
    {
        switch (state_)
        {
        case NFCState::READING_UID:
            pollUID();
            break;
        case NFCState::DUMPING:
            stepDump();
            break;
        default:
            break;
        }
    }

    void onDraw() override
    {
        if (!needsRedraw_ && !statusBar_.isDirty() && !mainMenu_.isDirty() && !progressBar_.isDirty())
        {
            return;
        }

        DisplayManager::instance().clear();
        statusBar_.draw();

        switch (state_)
        {
        case NFCState::MAIN_MENU:
            drawTitle("NFC Tools");
            mainMenu_.draw();
            break;
        case NFCState::READING_UID:
            drawTitle("Read UID");
            drawUIDView();
            break;
        case NFCState::DUMPING:
            drawTitle("Dump Mifare");
            drawDumpView();
            progressBar_.draw();
            break;
        case NFCState::DUMP_DONE:
            drawTitle("Dump Done");
            drawDumpDone();
            break;
        }

        DisplayManager::instance().present();
        statusBar_.clearDirty();
        mainMenu_.clearDirty();
        progressBar_.clearDirty();
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
        NFCReader::instance().deinit();
        EventSystem::instance().unsubscribe(this);
        ESP_LOGI(TAG_NFC_APP, "destroyed");
    }

private:
    static constexpr uint8_t UID_BUF_LEN = 7U;
    static constexpr size_t STATUS_LEN = 28U;

    StatusBar statusBar_;
    MenuListView mainMenu_;
    ProgressBar progressBar_;
    NFCState state_;
    bool needsRedraw_;
    uint8_t uid_[UID_BUF_LEN];
    uint8_t uidLen_;
    uint8_t dumpSector_;
    uint8_t dumpSuccess_;
    uint8_t dumpFail_;
    char statusLine_[STATUS_LEN];

    void transitionTo(NFCState next)
    {
        state_ = next;
        needsRedraw_ = true;
    }

    void drawTitle(const char *title)
    {
        DisplayManager::instance().drawText(2, 10, title);
        DisplayManager::instance().drawLine(0, 18, 127, 18);
    }

    void drawUIDView()
    {
        if (uidLen_ == 0U)
        {
            DisplayManager::instance().drawText(2, 30, "Waiting for card...");
        }
        else
        {
            // Build "XX:XX:XX:XX" UID string in one pass
            char hexUID[22] = {};
            size_t pos = 0U;
            for (uint8_t i = 0U; i < uidLen_ && i < UID_BUF_LEN; ++i)
            {
                const int written = std::snprintf(hexUID + pos, sizeof(hexUID) - pos,
                                                  i > 0U ? ":%02X" : "%02X",
                                                  static_cast<unsigned>(uid_[i]));
                if (written > 0)
                {
                    pos += static_cast<size_t>(written);
                }
            }
            DisplayManager::instance().drawText(2, 28, "UID:");
            DisplayManager::instance().drawText(2, 40, hexUID);
        }
    }

    void drawDumpView()
    {
        char line[24];
        std::snprintf(line, sizeof(line), "Sector %u/%u",
                      static_cast<unsigned>(dumpSector_),
                      static_cast<unsigned>(NFCReader::MIFARE_1K_SECTORS));
        DisplayManager::instance().drawText(2, 24, line);
        DisplayManager::instance().drawText(2, 36, statusLine_);
    }

    void drawDumpDone()
    {
        char line[24];
        std::snprintf(line, sizeof(line), "OK:%u Fail:%u",
                      static_cast<unsigned>(dumpSuccess_),
                      static_cast<unsigned>(dumpFail_));
        DisplayManager::instance().drawText(2, 28, line);
        DisplayManager::instance().drawText(2, 40, "Press to exit");
    }

    // ── NFC polling ────────────────────────────────────────────────────────────

    void pollUID()
    {
        uint8_t buf[UID_BUF_LEN] = {};
        uint8_t len = 0U;
        if (NFCReader::instance().readUID(buf, &len, 200U))
        {
            std::memcpy(uid_, buf, sizeof(uid_));
            uidLen_ = len;
            needsRedraw_ = true;
            ESP_LOGI(TAG_NFC_APP, "UID read, len=%u", static_cast<unsigned>(len));
        }
    }

    void stepDump()
    {
        if (dumpSector_ >= NFCReader::MIFARE_1K_SECTORS)
        {
            transitionTo(NFCState::DUMP_DONE);
            return;
        }

        const uint8_t firstBlock = static_cast<uint8_t>(dumpSector_ * NFCReader::BLOCKS_PER_SECTOR);

        if (NFCReader::instance().authenticateBlock(uid_, uidLen_, firstBlock))
        {
            bool sectorOk = true;
            for (uint8_t b = 0U; b < NFCReader::BLOCKS_PER_SECTOR; ++b)
            {
                uint8_t data[NFCReader::BYTES_PER_BLOCK] = {};
                if (!NFCReader::instance().readBlock(firstBlock + b, data))
                {
                    sectorOk = false;
                    break;
                }
                ESP_LOGD(TAG_NFC_APP, "S%u B%u: %02X%02X%02X%02X...",
                         static_cast<unsigned>(dumpSector_),
                         static_cast<unsigned>(b),
                         data[0], data[1], data[2], data[3]);
            }
            if (sectorOk)
            {
                ++dumpSuccess_;
                std::snprintf(statusLine_, sizeof(statusLine_), "S%u OK", static_cast<unsigned>(dumpSector_));
            }
            else
            {
                ++dumpFail_;
                std::snprintf(statusLine_, sizeof(statusLine_), "S%u read fail", static_cast<unsigned>(dumpSector_));
            }
        }
        else
        {
            ++dumpFail_;
            std::snprintf(statusLine_, sizeof(statusLine_), "S%u auth fail", static_cast<unsigned>(dumpSector_));
        }

        ++dumpSector_;
        const uint8_t pct = static_cast<uint8_t>((static_cast<uint16_t>(dumpSector_) * 100U) /
                                                   NFCReader::MIFARE_1K_SECTORS);
        progressBar_.setProgress(pct);
        needsRedraw_ = true;
    }

    // ── Input handling ─────────────────────────────────────────────────────────

    void handleInput(InputManager::InputEvent input)
    {
        switch (state_)
        {
        case NFCState::MAIN_MENU:
            handleMainMenu(input);
            break;
        case NFCState::READING_UID:
            if (input == InputManager::InputEvent::BUTTON_PRESS ||
                input == InputManager::InputEvent::LEFT)
            {
                uidLen_ = 0U;
                transitionTo(NFCState::MAIN_MENU);
                mainMenu_.setItems(NFC_MENU_LABELS, NFC_MENU_COUNT);
            }
            break;
        case NFCState::DUMPING:
            if (input == InputManager::InputEvent::BUTTON_PRESS ||
                input == InputManager::InputEvent::LEFT)
            {
                dumpSector_ = NFCReader::MIFARE_1K_SECTORS; // abort
                transitionTo(NFCState::DUMP_DONE);
            }
            break;
        case NFCState::DUMP_DONE:
            if (input == InputManager::InputEvent::BUTTON_PRESS ||
                input == InputManager::InputEvent::LEFT)
            {
                transitionTo(NFCState::MAIN_MENU);
                mainMenu_.setItems(NFC_MENU_LABELS, NFC_MENU_COUNT);
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
            if (sel == 0U) // Read UID
            {
                if (!NFCReader::instance().isReady())
                {
                    NFCReader::instance().init();
                }
                uidLen_ = 0U;
                transitionTo(NFCState::READING_UID);
            }
            else if (sel == 1U) // Dump Mifare
            {
                if (!NFCReader::instance().isReady())
                {
                    NFCReader::instance().init();
                }
                // Require UID first
                if (uidLen_ == 0U)
                {
                    uint8_t buf[UID_BUF_LEN] = {};
                    uint8_t len = 0U;
                    if (NFCReader::instance().readUID(buf, &len, 1000U))
                    {
                        std::memcpy(uid_, buf, sizeof(uid_));
                        uidLen_ = len;
                    }
                }
                if (uidLen_ > 0U)
                {
                    dumpSector_ = 0U;
                    dumpSuccess_ = 0U;
                    dumpFail_ = 0U;
                    progressBar_.setProgress(0U);
                    std::snprintf(statusLine_, sizeof(statusLine_), "Starting...");
                    transitionTo(NFCState::DUMPING);
                }
                else
                {
                    std::snprintf(statusLine_, sizeof(statusLine_), "No card found");
                    needsRedraw_ = true;
                }
            }
            else // Back
            {
                const Event evt{EventType::EVT_SYSTEM, SYSTEM_EVENT_BACK, 0, nullptr};
                EventSystem::instance().postEvent(evt);
            }
        }
    }
};

} // namespace

AppBase *createNFCToolsApp()
{
    return new (std::nothrow) NFCToolsApp();
}
