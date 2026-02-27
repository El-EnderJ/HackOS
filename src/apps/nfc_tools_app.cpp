/**
 * @file nfc_tools_app.cpp
 * @brief Phase 11 – NFC Tools: UID Cloner, Sector Dumper, Hex Viewer,
 *        NFC Tag Emulator.
 *
 * Features:
 *  - **UID Cloner**: Read UID (4/7 bytes), save, write to Magic Gen1/Gen2.
 *  - **Sector Dumper**: Multi-key Mifare Classic 1K dump with block matrix.
 *  - **Hex Viewer**: Scrollable hex display of dump data on OLED.
 *  - **NFC Emulator**: NTAG213 URL tag emulation (Rickroll, custom URLs).
 */

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
#include "hardware/storage.h"
#include "ui/widgets.h"

static constexpr const char *TAG_NFC_APP = "NFCToolsApp";

namespace
{

// ── App states ──────────────────────────────────────────────────────────────

enum class NFCState : uint8_t
{
    MAIN_MENU,
    READING_UID,
    UID_CLONER_MENU,
    WRITING_UID,
    DUMPING,
    DUMP_DONE,
    HEX_VIEWER,
    EMULATOR_MENU,
    EMULATING,
};

// ── Menu labels ─────────────────────────────────────────────────────────────

static constexpr size_t NFC_MENU_COUNT = 5U;
static const char *const NFC_MENU_LABELS[NFC_MENU_COUNT] = {
    "UID Cloner",
    "Sector Dump",
    "NFC Emulator",
    "Save UID",
    "Back",
};

static constexpr size_t CLONER_MENU_COUNT = 3U;
static const char *const CLONER_MENU_LABELS[CLONER_MENU_COUNT] = {
    "Read UID",
    "Write to Magic",
    "Back",
};

static constexpr size_t EMULATOR_MENU_COUNT = 4U;
static const char *const EMULATOR_MENU_LABELS[EMULATOR_MENU_COUNT] = {
    "Rickroll URL",
    "Example.com",
    "Custom URL",
    "Back",
};

// ── NDEF URI prefix codes ───────────────────────────────────────────────────
static constexpr uint8_t NDEF_PREFIX_HTTPS = 0x04U; // "https://"

// ── Predefined emulation URLs ───────────────────────────────────────────────
static const char *const EMULATOR_URLS[] = {
    "www.youtube.com/watch?v=dQw4w9WgXcQ", // Rickroll
    "www.example.com",                       // Example
};
static const uint8_t EMULATOR_PREFIXES[] = {
    NDEF_PREFIX_HTTPS,
    NDEF_PREFIX_HTTPS,
};

// ── Mifare dump constants ───────────────────────────────────────────────────
static constexpr size_t DUMP_BUF_SIZE =
    static_cast<size_t>(NFCReader::MIFARE_1K_BLOCKS) * NFCReader::BYTES_PER_BLOCK;

class NFCToolsApp final : public AppBase, public IEventObserver
{
public:
    NFCToolsApp()
        : statusBar_(0, 0, 128, 8),
          mainMenu_(0, 20, 128, 36, 3),
          subMenu_(0, 20, 128, 36, 3),
          progressBar_(0, 54, 128, 10),
          state_(NFCState::MAIN_MENU),
          needsRedraw_(true),
          uid_{},
          uidLen_(0U),
          dumpSector_(0U),
          dumpSuccess_(0U),
          dumpFail_(0U),
          blockStatus_{},
          dumpBuf_{},
          hexViewOffset_(0U),
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
        if (!needsRedraw_ && !statusBar_.isDirty() &&
            !mainMenu_.isDirty() && !subMenu_.isDirty() &&
            !progressBar_.isDirty())
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
        case NFCState::UID_CLONER_MENU:
            drawTitle("UID Cloner");
            drawUIDPreview();
            subMenu_.draw();
            break;
        case NFCState::WRITING_UID:
            drawTitle("Write UID");
            drawStatus();
            break;
        case NFCState::DUMPING:
            drawTitle("Sector Dump");
            drawDumpView();
            drawBlockMatrix();
            progressBar_.draw();
            break;
        case NFCState::DUMP_DONE:
            drawTitle("Dump Done");
            drawDumpDone();
            drawBlockMatrix();
            break;
        case NFCState::HEX_VIEWER:
            drawTitle("Hex Viewer");
            drawHexViewer();
            break;
        case NFCState::EMULATOR_MENU:
            drawTitle("NFC Emulator");
            subMenu_.draw();
            break;
        case NFCState::EMULATING:
            drawTitle("Emulating");
            drawStatus();
            break;
        }

        DisplayManager::instance().present();
        statusBar_.clearDirty();
        mainMenu_.clearDirty();
        subMenu_.clearDirty();
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
    static constexpr size_t STATUS_LEN = 32U;

    StatusBar statusBar_;
    MenuListView mainMenu_;
    MenuListView subMenu_;
    ProgressBar progressBar_;
    NFCState state_;
    bool needsRedraw_;
    uint8_t uid_[UID_BUF_LEN];
    uint8_t uidLen_;
    uint8_t dumpSector_;
    uint8_t dumpSuccess_;
    uint8_t dumpFail_;
    /// Per-block status bitmap: 1 = success, 0 = fail/unread.
    uint8_t blockStatus_[NFCReader::MIFARE_1K_BLOCKS / 8U + 1U];
    /// Sector dump data buffer.
    uint8_t dumpBuf_[DUMP_BUF_SIZE];
    /// Hex viewer scroll offset (in blocks).
    uint8_t hexViewOffset_;
    char statusLine_[STATUS_LEN];

    // ── Helpers ─────────────────────────────────────────────────────────────

    void transitionTo(NFCState next)
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
                ESP_LOGE(TAG_NFC_APP, "PN532 init failed");
                return false;
            }
        }
        return true;
    }

    void setBlockStatus(uint8_t block, bool ok)
    {
        if (block < NFCReader::MIFARE_1K_BLOCKS)
        {
            const uint8_t byteIdx = block / 8U;
            const uint8_t bitIdx = block % 8U;
            if (ok)
            {
                blockStatus_[byteIdx] |= static_cast<uint8_t>(1U << bitIdx);
            }
            else
            {
                blockStatus_[byteIdx] &= static_cast<uint8_t>(~(1U << bitIdx));
            }
        }
    }

    bool getBlockStatus(uint8_t block) const
    {
        if (block >= NFCReader::MIFARE_1K_BLOCKS)
        {
            return false;
        }
        return (blockStatus_[block / 8U] & (1U << (block % 8U))) != 0U;
    }

    static void formatUid(const uint8_t *uid, uint8_t len, char *out, size_t outLen)
    {
        size_t pos = 0U;
        for (uint8_t i = 0U; i < len && i < 7U; ++i)
        {
            const int written = std::snprintf(out + pos, outLen - pos,
                                              i > 0U ? ":%02X" : "%02X",
                                              static_cast<unsigned>(uid[i]));
            if (written > 0 && static_cast<size_t>(written) < outLen - pos)
            {
                pos += static_cast<size_t>(written);
            }
        }
    }

    // ── Drawing ─────────────────────────────────────────────────────────────

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
            char hexUID[22] = {};
            formatUid(uid_, uidLen_, hexUID, sizeof(hexUID));
            DisplayManager::instance().drawText(2, 28, "UID:");
            DisplayManager::instance().drawText(2, 40, hexUID);
            DisplayManager::instance().drawText(2, 54, "OK - press to return");
        }
    }

    void drawUIDPreview()
    {
        if (uidLen_ > 0U)
        {
            char hexUID[22] = {};
            formatUid(uid_, uidLen_, hexUID, sizeof(hexUID));
            char line[28];
            std::snprintf(line, sizeof(line), "UID:%s", hexUID);
            DisplayManager::instance().drawText(2, 20, line);
        }
    }

    void drawStatus()
    {
        DisplayManager::instance().drawText(2, 32, statusLine_);
    }

    void drawDumpView()
    {
        char line[24];
        std::snprintf(line, sizeof(line), "Sector %u/%u",
                      static_cast<unsigned>(dumpSector_),
                      static_cast<unsigned>(NFCReader::MIFARE_1K_SECTORS));
        DisplayManager::instance().drawText(2, 22, line);
        DisplayManager::instance().drawText(2, 32, statusLine_);
    }

    /// Draw a 16-column x 4-row matrix of blocks (64 blocks total for 1K).
    /// Each cell is a small rectangle: filled = success, outline = fail.
    void drawBlockMatrix()
    {
        static constexpr int16_t MX = 2;  // matrix X origin
        static constexpr int16_t MY = 40; // matrix Y origin
        static constexpr int16_t CW = 7;  // cell width  (incl. 1px gap)
        static constexpr int16_t CH = 3;  // cell height (incl. 1px gap)

        for (uint8_t b = 0U; b < NFCReader::MIFARE_1K_BLOCKS; ++b)
        {
            const int16_t col = static_cast<int16_t>(b % 16U);
            const int16_t row = static_cast<int16_t>(b / 16U);
            const int16_t cx = MX + col * CW;
            const int16_t cy = MY + row * CH;

            if (b < dumpSector_ * NFCReader::BLOCKS_PER_SECTOR)
            {
                if (getBlockStatus(b))
                {
                    DisplayManager::instance().fillRect(cx, cy, CW - 1, CH - 1);
                }
                else
                {
                    DisplayManager::instance().drawRect(cx, cy, CW - 1, CH - 1);
                }
            }
        }
    }

    void drawDumpDone()
    {
        char line[24];
        std::snprintf(line, sizeof(line), "OK:%u Fail:%u",
                      static_cast<unsigned>(dumpSuccess_),
                      static_cast<unsigned>(dumpFail_));
        DisplayManager::instance().drawText(2, 22, line);
        DisplayManager::instance().drawText(2, 32, "UP/DN:Hex  OK:Exit");
    }

    /// Scrollable hex viewer: shows 3 rows of hex data at a time.
    void drawHexViewer()
    {
        static constexpr uint8_t VISIBLE_ROWS = 3U;
        const uint8_t maxOffset = (NFCReader::MIFARE_1K_BLOCKS > VISIBLE_ROWS)
                                      ? (NFCReader::MIFARE_1K_BLOCKS - VISIBLE_ROWS)
                                      : 0U;
        if (hexViewOffset_ > maxOffset)
        {
            hexViewOffset_ = maxOffset;
        }

        for (uint8_t r = 0U; r < VISIBLE_ROWS; ++r)
        {
            const uint8_t block = hexViewOffset_ + r;
            if (block >= NFCReader::MIFARE_1K_BLOCKS)
            {
                break;
            }

            const size_t offset = static_cast<size_t>(block) * NFCReader::BYTES_PER_BLOCK;

            // Line format: "BB:XXXXXXXXXXXXXXXX" (block# + first 8 hex bytes)
            char line[28];
            if (getBlockStatus(block))
            {
                std::snprintf(line, sizeof(line),
                              "%02X:%02X%02X%02X%02X%02X%02X%02X%02X",
                              static_cast<unsigned>(block),
                              dumpBuf_[offset + 0U], dumpBuf_[offset + 1U],
                              dumpBuf_[offset + 2U], dumpBuf_[offset + 3U],
                              dumpBuf_[offset + 4U], dumpBuf_[offset + 5U],
                              dumpBuf_[offset + 6U], dumpBuf_[offset + 7U]);
            }
            else
            {
                std::snprintf(line, sizeof(line), "%02X:-- auth fail --",
                              static_cast<unsigned>(block));
            }

            const int16_t y = static_cast<int16_t>(22 + r * 12);
            DisplayManager::instance().drawText(2, y, line);
        }

        // Scroll indicator
        char indicator[16];
        std::snprintf(indicator, sizeof(indicator), "Blk %u-%u/%u",
                      static_cast<unsigned>(hexViewOffset_),
                      static_cast<unsigned>(hexViewOffset_ + VISIBLE_ROWS - 1U),
                      static_cast<unsigned>(NFCReader::MIFARE_1K_BLOCKS - 1U));
        DisplayManager::instance().drawText(2, 58, indicator);
    }

    // ── NFC operations ──────────────────────────────────────────────────────

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

        // Try multiple default keys for authentication
        if (NFCReader::instance().authenticateBlockWithKeys(uid_, uidLen_, firstBlock))
        {
            bool sectorOk = true;
            for (uint8_t b = 0U; b < NFCReader::BLOCKS_PER_SECTOR; ++b)
            {
                const uint8_t blockAddr = firstBlock + b;
                const size_t bufOffset = static_cast<size_t>(blockAddr) * NFCReader::BYTES_PER_BLOCK;
                uint8_t *dst = dumpBuf_ + bufOffset;

                if (NFCReader::instance().readBlock(blockAddr, dst))
                {
                    setBlockStatus(blockAddr, true);
                    ESP_LOGD(TAG_NFC_APP, "S%u B%u: %02X%02X%02X%02X...",
                             static_cast<unsigned>(dumpSector_),
                             static_cast<unsigned>(b),
                             dst[0], dst[1], dst[2], dst[3]);
                }
                else
                {
                    sectorOk = false;
                    setBlockStatus(blockAddr, false);
                    break;
                }
            }
            if (sectorOk)
            {
                ++dumpSuccess_;
                std::snprintf(statusLine_, sizeof(statusLine_), "S%u OK",
                              static_cast<unsigned>(dumpSector_));
            }
            else
            {
                ++dumpFail_;
                std::snprintf(statusLine_, sizeof(statusLine_), "S%u read fail",
                              static_cast<unsigned>(dumpSector_));
            }
        }
        else
        {
            ++dumpFail_;
            for (uint8_t b = 0U; b < NFCReader::BLOCKS_PER_SECTOR; ++b)
            {
                setBlockStatus(firstBlock + b, false);
            }
            std::snprintf(statusLine_, sizeof(statusLine_), "S%u auth fail",
                          static_cast<unsigned>(dumpSector_));
        }

        ++dumpSector_;
        const uint8_t pct = static_cast<uint8_t>(
            (static_cast<uint16_t>(dumpSector_) * 100U) / NFCReader::MIFARE_1K_SECTORS);
        progressBar_.setProgress(pct);
        needsRedraw_ = true;
    }

    void startWriteUid()
    {
        if (uidLen_ == 0U)
        {
            std::snprintf(statusLine_, sizeof(statusLine_), "No UID stored");
            transitionTo(NFCState::WRITING_UID);
            return;
        }

        std::snprintf(statusLine_, sizeof(statusLine_), "Writing UID...");
        needsRedraw_ = true;
        transitionTo(NFCState::WRITING_UID);

        if (NFCReader::instance().writeMagicUid(uid_, uidLen_))
        {
            std::snprintf(statusLine_, sizeof(statusLine_), "UID written OK!");
        }
        else
        {
            std::snprintf(statusLine_, sizeof(statusLine_), "Write failed");
        }
        needsRedraw_ = true;
    }

    void startEmulation(size_t urlIdx)
    {
        if (urlIdx >= (sizeof(EMULATOR_URLS) / sizeof(EMULATOR_URLS[0])))
        {
            return;
        }

        std::snprintf(statusLine_, sizeof(statusLine_), "Emulating...");
        transitionTo(NFCState::EMULATING);

        const bool ok = NFCReader::instance().emulateNtag213Url(
            EMULATOR_URLS[urlIdx], EMULATOR_PREFIXES[urlIdx], 30000U);

        std::snprintf(statusLine_, sizeof(statusLine_),
                      ok ? "Tag served OK" : "Timeout/no reader");
        needsRedraw_ = true;
    }

    // ── Input handling ──────────────────────────────────────────────────────

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
                transitionTo(NFCState::UID_CLONER_MENU);
                subMenu_.setItems(CLONER_MENU_LABELS, CLONER_MENU_COUNT);
            }
            break;
        case NFCState::UID_CLONER_MENU:
            handleClonerMenu(input);
            break;
        case NFCState::WRITING_UID:
            if (input == InputManager::InputEvent::BUTTON_PRESS ||
                input == InputManager::InputEvent::LEFT)
            {
                transitionTo(NFCState::UID_CLONER_MENU);
                subMenu_.setItems(CLONER_MENU_LABELS, CLONER_MENU_COUNT);
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
            handleDumpDone(input);
            break;
        case NFCState::HEX_VIEWER:
            handleHexViewer(input);
            break;
        case NFCState::EMULATOR_MENU:
            handleEmulatorMenu(input);
            break;
        case NFCState::EMULATING:
            if (input == InputManager::InputEvent::BUTTON_PRESS ||
                input == InputManager::InputEvent::LEFT)
            {
                transitionTo(NFCState::EMULATOR_MENU);
                subMenu_.setItems(EMULATOR_MENU_LABELS, EMULATOR_MENU_COUNT);
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
            if (sel == 0U) // UID Cloner
            {
                if (!ensureNfcReady()) { return; }
                subMenu_.setItems(CLONER_MENU_LABELS, CLONER_MENU_COUNT);
                transitionTo(NFCState::UID_CLONER_MENU);
            }
            else if (sel == 1U) // Sector Dump
            {
                if (!ensureNfcReady()) { return; }
                startSectorDump();
            }
            else if (sel == 2U) // NFC Emulator
            {
                if (!ensureNfcReady()) { return; }
                subMenu_.setItems(EMULATOR_MENU_LABELS, EMULATOR_MENU_COUNT);
                transitionTo(NFCState::EMULATOR_MENU);
            }
            else if (sel == 3U) // Save UID
            {
                saveUidToSd();
            }
            else // Back
            {
                const Event evt{EventType::EVT_SYSTEM, SYSTEM_EVENT_BACK, 0, nullptr};
                EventSystem::instance().postEvent(evt);
            }
        }
    }

    void handleClonerMenu(InputManager::InputEvent input)
    {
        if (input == InputManager::InputEvent::UP)
        {
            subMenu_.moveSelection(-1);
        }
        else if (input == InputManager::InputEvent::DOWN)
        {
            subMenu_.moveSelection(1);
        }
        else if (input == InputManager::InputEvent::BUTTON_PRESS)
        {
            const size_t sel = subMenu_.selectedIndex();
            if (sel == 0U) // Read UID
            {
                uidLen_ = 0U;
                transitionTo(NFCState::READING_UID);
            }
            else if (sel == 1U) // Write to Magic
            {
                startWriteUid();
            }
            else // Back
            {
                transitionTo(NFCState::MAIN_MENU);
                mainMenu_.setItems(NFC_MENU_LABELS, NFC_MENU_COUNT);
            }
        }
        else if (input == InputManager::InputEvent::LEFT)
        {
            transitionTo(NFCState::MAIN_MENU);
            mainMenu_.setItems(NFC_MENU_LABELS, NFC_MENU_COUNT);
        }
    }

    void handleEmulatorMenu(InputManager::InputEvent input)
    {
        if (input == InputManager::InputEvent::UP)
        {
            subMenu_.moveSelection(-1);
        }
        else if (input == InputManager::InputEvent::DOWN)
        {
            subMenu_.moveSelection(1);
        }
        else if (input == InputManager::InputEvent::BUTTON_PRESS)
        {
            const size_t sel = subMenu_.selectedIndex();
            if (sel < 2U) // Predefined URLs
            {
                startEmulation(sel);
            }
            else if (sel == 2U) // Custom URL – placeholder (no OLED text input yet)
            {
                startEmulation(0U); // defaults to Rickroll
            }
            else // Back
            {
                transitionTo(NFCState::MAIN_MENU);
                mainMenu_.setItems(NFC_MENU_LABELS, NFC_MENU_COUNT);
            }
        }
        else if (input == InputManager::InputEvent::LEFT)
        {
            transitionTo(NFCState::MAIN_MENU);
            mainMenu_.setItems(NFC_MENU_LABELS, NFC_MENU_COUNT);
        }
    }

    void handleDumpDone(InputManager::InputEvent input)
    {
        if (input == InputManager::InputEvent::UP ||
            input == InputManager::InputEvent::DOWN)
        {
            hexViewOffset_ = 0U;
            transitionTo(NFCState::HEX_VIEWER);
        }
        else if (input == InputManager::InputEvent::BUTTON_PRESS ||
                 input == InputManager::InputEvent::LEFT)
        {
            transitionTo(NFCState::MAIN_MENU);
            mainMenu_.setItems(NFC_MENU_LABELS, NFC_MENU_COUNT);
        }
    }

    void handleHexViewer(InputManager::InputEvent input)
    {
        if (input == InputManager::InputEvent::UP)
        {
            if (hexViewOffset_ > 0U)
            {
                --hexViewOffset_;
                needsRedraw_ = true;
            }
        }
        else if (input == InputManager::InputEvent::DOWN)
        {
            const uint8_t maxScroll = (NFCReader::MIFARE_1K_BLOCKS > 3U)
                                         ? static_cast<uint8_t>(NFCReader::MIFARE_1K_BLOCKS - 3U)
                                         : 0U;
            if (hexViewOffset_ < maxScroll)
            {
                ++hexViewOffset_;
                needsRedraw_ = true;
            }
        }
        else if (input == InputManager::InputEvent::BUTTON_PRESS ||
                 input == InputManager::InputEvent::LEFT)
        {
            transitionTo(NFCState::DUMP_DONE);
        }
    }

    void startSectorDump()
    {
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
            std::memset(blockStatus_, 0, sizeof(blockStatus_));
            std::memset(dumpBuf_, 0, sizeof(dumpBuf_));
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

    void saveUidToSd()
    {
        if (uidLen_ == 0U)
        {
            ESP_LOGW(TAG_NFC_APP, "saveUidToSd: no UID");
            return;
        }
        if (!StorageManager::instance().isMounted())
        {
            ESP_LOGW(TAG_NFC_APP, "saveUidToSd: SD not mounted");
            return;
        }

        char hexUID[22] = {};
        formatUid(uid_, uidLen_, hexUID, sizeof(hexUID));

        char line[32];
        const int len = std::snprintf(line, sizeof(line), "UID: %s\n", hexUID);
        if (len > 0 && len < static_cast<int>(sizeof(line)))
        {
            const bool ok = StorageManager::instance().appendChunk(
                "/captures/nfc_uid.txt",
                reinterpret_cast<const uint8_t *>(line),
                static_cast<size_t>(len));
            ESP_LOGI(TAG_NFC_APP, "saveUidToSd: %s", ok ? "OK" : "FAIL");
        }
    }
};

} // namespace

AppBase *createNFCToolsApp()
{
    return new (std::nothrow) NFCToolsApp();
}
