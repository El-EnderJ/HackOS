/**
 * @file ir_tools_app.cpp
 * @brief Phase 10 – IR Tools: TV-B-Gone, Protocol Auto-Detect, Signal Editor,
 *        DB Manager.
 *
 * Features:
 *  - **TV-B-Gone (Universal Remote)**: Reads `/ext/assets/ir/tv_bgone.csv`
 *    and sequentially transmits Power codes for 50+ brands with 200 ms delay.
 *  - **Protocol Auto-Detect (Sniffer)**: Receives an IR signal on GPIO15,
 *    identifies the protocol (NEC, Sony, Samsung, RC5, etc.), and displays
 *    the hex value and bit length.
 *  - **Signal Editor**: Allows editing a captured hex code before re-sending.
 *  - **DB Manager**: Name-and-save captured codes to SD for future replay.
 */

#include "apps/ir_tools_app.h"

#include <IRutils.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <esp_log.h>
#include <new>

#include "config.h"
#include "core/event.h"
#include "core/event_system.h"
#include "hardware/display.h"
#include "hardware/input.h"
#include "hardware/ir_transceiver.h"
#include "hardware/storage.h"
#include "storage/vfs.h"
#include "ui/widgets.h"

static constexpr const char *TAG_IR_APP = "IRToolsApp";

namespace
{

// ── App states ──────────────────────────────────────────────────────────────

enum class IRState : uint8_t
{
    MAIN_MENU,
    SNIFFER,
    CLONER,
    TV_BGONE_RUNNING,
    TV_BGONE_DONE,
    SIGNAL_EDITOR,
    DB_SAVE,
    DB_LOAD_LIST,
    DB_LOADED,
};

// ── Menu labels ─────────────────────────────────────────────────────────────

static constexpr size_t IR_MENU_COUNT = 7U;
static const char *const IR_MENU_LABELS[IR_MENU_COUNT] = {
    "TV-B-Gone",
    "IR Sniffer",
    "IR Cloner",
    "Signal Editor",
    "Save to DB",
    "Load from DB",
    "Back",
};

// ── TV-B-Gone constants ─────────────────────────────────────────────────────

/// Path to the TV-B-Gone CSV database on the SD card.
static constexpr const char *TV_BGONE_PATH = "/ext/assets/ir/tv_bgone.csv";

/// Delay between sequential code transmissions (milliseconds).
static constexpr uint32_t TV_BGONE_DELAY_MS = 200U;

/// Maximum number of codes to store from the CSV.
static constexpr size_t TV_BGONE_MAX_CODES = 64U;

/// Maximum CSV line length.
static constexpr size_t CSV_LINE_MAX = 128U;

// ── DB Manager constants ────────────────────────────────────────────────────

/// Base directory for user-saved IR codes.
static constexpr const char *IR_SAVED_DIR = "/ext/assets/ir/saved";

/// Maximum saved entries shown in the load list.
static constexpr size_t DB_MAX_ENTRIES = 16U;

// ── Signal Editor constants ─────────────────────────────────────────────────

/// Number of hex nibbles editable (8 nibbles = 32-bit code).
static constexpr size_t HEX_NIBBLES = 8U;

// ── TV-B-Gone entry ─────────────────────────────────────────────────────────

struct TvBGoneEntry
{
    char brand[20];
    decode_type_t protocol;
    uint64_t code;
    uint16_t bits;
};

// ═════════════════════════════════════════════════════════════════════════════
// ── IRToolsApp ──────────────────────────────────────────────────────────────
// ═════════════════════════════════════════════════════════════════════════════

class IRToolsApp final : public AppBase, public IEventObserver
{
public:
    IRToolsApp()
        : statusBar_(0, 0, 128, 8),
          mainMenu_(0, 20, 128, 36, 3),
          dbMenu_(0, 20, 128, 36, 3),
          state_(IRState::MAIN_MENU),
          needsRedraw_(true),
          clonerSent_(false),
          protoName_{},
          codeHex_{},
          tvbEntryCount_(0U),
          tvbCurrent_(0U),
          tvbRunning_(false),
          tvbLastSendMs_(0U),
          editNibbleIdx_(0U),
          editValue_(0U),
          editBits_(32U),
          editProto_(static_cast<decode_type_t>(3)), // NEC default
          dbEntryCount_(0U),
          saveName_{},
          saveNameLen_(0U),
          saveNameCursorChar_('A')
    {
        std::memset(tvbEntries_, 0, sizeof(tvbEntries_));
        std::memset(dbEntryNames_, 0, sizeof(dbEntryNames_));
    }

    // ── AppBase lifecycle ────────────────────────────────────────────────

    void onSetup() override
    {
        statusBar_.setConnectivity(false, false);
        statusBar_.setBatteryLevel(100U);
        statusBar_.setTime(0U, 0U);
        mainMenu_.setItems(IR_MENU_LABELS, IR_MENU_COUNT);
        (void)EventSystem::instance().subscribe(this);
        state_ = IRState::MAIN_MENU;
        needsRedraw_ = true;
        ESP_LOGI(TAG_IR_APP, "setup");
    }

    void onLoop() override
    {
        switch (state_)
        {
        case IRState::SNIFFER:
            pollSniffer();
            break;
        case IRState::CLONER:
            if (!clonerSent_)
            {
                performClone();
            }
            break;
        case IRState::TV_BGONE_RUNNING:
            pollTvBGone();
            break;
        default:
            break;
        }
    }

    void onDraw() override
    {
        if (!needsRedraw_ && !statusBar_.isDirty() && !mainMenu_.isDirty()
            && !dbMenu_.isDirty())
        {
            return;
        }

        DisplayManager::instance().clear();
        statusBar_.draw();

        switch (state_)
        {
        case IRState::MAIN_MENU:
            drawTitle("IR Tools");
            mainMenu_.draw();
            break;
        case IRState::SNIFFER:
            drawTitle("IR Auto-Detect");
            drawSnifferView();
            break;
        case IRState::CLONER:
            drawTitle("IR Cloner");
            drawClonerView();
            break;
        case IRState::TV_BGONE_RUNNING:
            drawTitle("TV-B-Gone");
            drawTvBGoneView();
            break;
        case IRState::TV_BGONE_DONE:
            drawTitle("TV-B-Gone");
            drawTvBGoneDone();
            break;
        case IRState::SIGNAL_EDITOR:
            drawTitle("Signal Editor");
            drawEditorView();
            break;
        case IRState::DB_SAVE:
            drawTitle("Save to DB");
            drawDbSaveView();
            break;
        case IRState::DB_LOAD_LIST:
            drawTitle("Load from DB");
            dbMenu_.draw();
            break;
        case IRState::DB_LOADED:
            drawTitle("Loaded");
            drawDbLoadedView();
            break;
        }

        DisplayManager::instance().present();
        statusBar_.clearDirty();
        mainMenu_.clearDirty();
        dbMenu_.clearDirty();
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
        IRTransceiver::instance().deinit();
        EventSystem::instance().unsubscribe(this);
        ESP_LOGI(TAG_IR_APP, "destroyed");
    }

private:
    StatusBar statusBar_;
    MenuListView mainMenu_;
    MenuListView dbMenu_;
    IRState state_;
    bool needsRedraw_;
    bool clonerSent_;
    char protoName_[16];
    char codeHex_[20];

    // ── TV-B-Gone state ─────────────────────────────────────────────────
    TvBGoneEntry tvbEntries_[TV_BGONE_MAX_CODES];
    size_t tvbEntryCount_;
    size_t tvbCurrent_;
    bool tvbRunning_;
    uint32_t tvbLastSendMs_;

    // ── Signal Editor state ─────────────────────────────────────────────
    size_t editNibbleIdx_;
    uint64_t editValue_;
    uint16_t editBits_;
    decode_type_t editProto_;

    // ── DB Manager state ────────────────────────────────────────────────
    char dbEntryNames_[DB_MAX_ENTRIES][20];
    const char *dbMenuLabels_[DB_MAX_ENTRIES];
    size_t dbEntryCount_;

    /// Name being composed in DB_SAVE state.
    char saveName_[16];
    size_t saveNameLen_;
    char saveNameCursorChar_;

    // ── State transitions ───────────────────────────────────────────────

    void transitionTo(IRState next)
    {
        state_ = next;
        needsRedraw_ = true;
    }

    // ── Drawing helpers ─────────────────────────────────────────────────

    void drawTitle(const char *title)
    {
        DisplayManager::instance().drawText(2, 10, title);
        DisplayManager::instance().drawLine(0, 18, 127, 18);
    }

    // ── Sniffer (Protocol Auto-Detect) ──────────────────────────────────

    void drawSnifferView()
    {
        if (IRTransceiver::instance().hasLastCode())
        {
            DisplayManager::instance().drawText(2, 24, protoName_);
            DisplayManager::instance().drawText(2, 36, codeHex_);
            char bits[16];
            std::snprintf(bits, sizeof(bits), "Bits: %u",
                          static_cast<unsigned>(IRTransceiver::instance().lastBits()));
            DisplayManager::instance().drawText(2, 48, bits);
        }
        else
        {
            DisplayManager::instance().drawText(2, 30, "Waiting for IR...");
        }
    }

    void pollSniffer()
    {
        uint64_t value = 0U;
        decode_type_t proto = decode_type_t::UNKNOWN;
        uint16_t bits = 0U;

        if (IRTransceiver::instance().decode(value, proto, bits))
        {
            const String &protoStr = typeToString(proto, false);
            std::snprintf(protoName_, sizeof(protoName_), "%.15s", protoStr.c_str());
            std::snprintf(codeHex_, sizeof(codeHex_), "0x%08llX",
                          static_cast<unsigned long long>(value));

            // Populate editor defaults from last capture
            editValue_ = value;
            editBits_ = bits;
            editProto_ = proto;

            needsRedraw_ = true;
            ESP_LOGI(TAG_IR_APP, "sniffed: %s 0x%llX %ubits",
                     protoName_,
                     static_cast<unsigned long long>(value),
                     static_cast<unsigned>(bits));
        }
    }

    // ── Cloner ──────────────────────────────────────────────────────────

    void drawClonerView()
    {
        if (!IRTransceiver::instance().hasLastCode())
        {
            DisplayManager::instance().drawText(2, 28, "No code captured.");
            DisplayManager::instance().drawText(2, 40, "Use Sniffer first");
        }
        else
        {
            DisplayManager::instance().drawText(2, 28, "Sending:");
            DisplayManager::instance().drawText(2, 40, codeHex_);
        }
    }

    void performClone()
    {
        if (!IRTransceiver::instance().hasLastCode())
        {
            return;
        }

        IRTransceiver::instance().send(
            IRTransceiver::instance().lastValue(),
            IRTransceiver::instance().lastProtocol(),
            IRTransceiver::instance().lastBits());

        ESP_LOGI(TAG_IR_APP, "cloned code sent");
        clonerSent_ = true;
        needsRedraw_ = true;
    }

    // ── TV-B-Gone ───────────────────────────────────────────────────────

    bool loadTvBGoneDb()
    {
        auto &vfs = hackos::storage::VirtualFS::instance();
        fs::File f = vfs.open(TV_BGONE_PATH, "r");
        if (!f)
        {
            ESP_LOGE(TAG_IR_APP, "Cannot open %s", TV_BGONE_PATH);
            return false;
        }

        tvbEntryCount_ = 0U;
        char line[CSV_LINE_MAX];

        while (f.available() && tvbEntryCount_ < TV_BGONE_MAX_CODES)
        {
            const size_t len = f.readBytesUntil('\n', line, sizeof(line) - 1U);
            if (len == 0U)
            {
                continue;
            }
            line[len] = '\0';

            // Skip comment lines
            if (line[0] == '#')
            {
                continue;
            }

            // Parse: brand,protocol_id,hex_code,bits
            char *saveptr = nullptr;
            char *brand = strtok_r(line, ",", &saveptr);
            char *protoStr = strtok_r(nullptr, ",", &saveptr);
            char *codeStr = strtok_r(nullptr, ",", &saveptr);
            char *bitsStr = strtok_r(nullptr, ",\r\n", &saveptr);

            if (brand == nullptr || protoStr == nullptr ||
                codeStr == nullptr || bitsStr == nullptr)
            {
                continue;
            }

            TvBGoneEntry &e = tvbEntries_[tvbEntryCount_];
            std::snprintf(e.brand, sizeof(e.brand), "%.19s", brand);
            e.protocol = static_cast<decode_type_t>(std::strtol(protoStr, nullptr, 10));
            e.code = std::strtoull(codeStr, nullptr, 16);
            e.bits = static_cast<uint16_t>(std::strtoul(bitsStr, nullptr, 10));

            ++tvbEntryCount_;
        }

        f.close();
        ESP_LOGI(TAG_IR_APP, "Loaded %u TV-B-Gone entries", static_cast<unsigned>(tvbEntryCount_));
        return tvbEntryCount_ > 0U;
    }

    void startTvBGone()
    {
        if (!loadTvBGoneDb())
        {
            ESP_LOGW(TAG_IR_APP, "TV-B-Gone DB empty or not found");
            return;
        }

        IRTransceiver::instance().initTransmit();
        tvbCurrent_ = 0U;
        tvbRunning_ = true;
        tvbLastSendMs_ = 0U;
        transitionTo(IRState::TV_BGONE_RUNNING);
    }

    void pollTvBGone()
    {
        if (!tvbRunning_)
        {
            return;
        }

        const uint32_t now = static_cast<uint32_t>(millis());
        if (now - tvbLastSendMs_ < TV_BGONE_DELAY_MS)
        {
            return;
        }

        if (tvbCurrent_ >= tvbEntryCount_)
        {
            // All codes sent
            tvbRunning_ = false;
            IRTransceiver::instance().deinit();
            transitionTo(IRState::TV_BGONE_DONE);
            ESP_LOGI(TAG_IR_APP, "TV-B-Gone complete");
            EventSystem::instance().postEvent(
                {EventType::EVT_XP_EARNED, XP_IR_SEND, 0, nullptr});
            return;
        }

        const TvBGoneEntry &e = tvbEntries_[tvbCurrent_];
        IRTransceiver::instance().send(e.code, e.protocol, e.bits);
        ESP_LOGD(TAG_IR_APP, "TV-B-Gone [%u/%u] %s proto=%d 0x%llX",
                 static_cast<unsigned>(tvbCurrent_ + 1U),
                 static_cast<unsigned>(tvbEntryCount_),
                 e.brand,
                 static_cast<int>(e.protocol),
                 static_cast<unsigned long long>(e.code));

        tvbLastSendMs_ = now;
        ++tvbCurrent_;
        needsRedraw_ = true;
    }

    void drawTvBGoneView()
    {
        if (tvbEntryCount_ == 0U)
        {
            DisplayManager::instance().drawText(2, 30, "No DB loaded");
            return;
        }

        char progress[32];
        std::snprintf(progress, sizeof(progress), "Sending %u/%u",
                      static_cast<unsigned>(tvbCurrent_),
                      static_cast<unsigned>(tvbEntryCount_));
        DisplayManager::instance().drawText(2, 24, progress);

        if (tvbCurrent_ > 0U && tvbCurrent_ <= tvbEntryCount_)
        {
            const TvBGoneEntry &e = tvbEntries_[tvbCurrent_ - 1U];
            DisplayManager::instance().drawText(2, 36, e.brand);

            char hexBuf[20];
            std::snprintf(hexBuf, sizeof(hexBuf), "0x%08llX",
                          static_cast<unsigned long long>(e.code));
            DisplayManager::instance().drawText(2, 48, hexBuf);
        }

        DisplayManager::instance().drawText(2, 58, "Press to cancel");
    }

    void drawTvBGoneDone()
    {
        char msg[32];
        std::snprintf(msg, sizeof(msg), "Done! Sent %u codes",
                      static_cast<unsigned>(tvbEntryCount_));
        DisplayManager::instance().drawText(2, 30, msg);
        DisplayManager::instance().drawText(2, 48, "Press to continue");
    }

    // ── Signal Editor ───────────────────────────────────────────────────

    void enterEditor()
    {
        if (IRTransceiver::instance().hasLastCode())
        {
            editValue_ = IRTransceiver::instance().lastValue();
            editBits_ = IRTransceiver::instance().lastBits();
            editProto_ = IRTransceiver::instance().lastProtocol();
        }
        editNibbleIdx_ = 0U;
        transitionTo(IRState::SIGNAL_EDITOR);
    }

    void drawEditorView()
    {
        // Show the protocol name
        const String &protoStr = typeToString(editProto_, false);
        char protoBuf[24];
        std::snprintf(protoBuf, sizeof(protoBuf), "Proto: %.15s", protoStr.c_str());
        DisplayManager::instance().drawText(2, 24, protoBuf);

        // Show editable hex value with cursor marker
        char hexBuf[24];
        std::snprintf(hexBuf, sizeof(hexBuf), "0x%08llX",
                      static_cast<unsigned long long>(editValue_));
        DisplayManager::instance().drawText(2, 36, hexBuf);

        // Draw cursor indicator (underline the active nibble)
        // hexBuf is "0x" + 8 chars, each char is ~6px wide
        const int16_t cursorX = static_cast<int16_t>(2 + (2U + editNibbleIdx_) * 6U);
        DisplayManager::instance().drawLine(cursorX, 38, cursorX + 5, 38);

        char bitsInfo[20];
        std::snprintf(bitsInfo, sizeof(bitsInfo), "Bits: %u",
                      static_cast<unsigned>(editBits_));
        DisplayManager::instance().drawText(2, 48, bitsInfo);

        DisplayManager::instance().drawText(2, 58, "U/D:edit L:curs OK:send");
    }

    void editorHandleInput(InputManager::InputEvent input)
    {
        if (input == InputManager::InputEvent::UP)
        {
            // Increment the nibble at current position
            const size_t shift = (HEX_NIBBLES - 1U - editNibbleIdx_) * 4U;
            uint8_t nibble = static_cast<uint8_t>((editValue_ >> shift) & 0xFU);
            nibble = static_cast<uint8_t>((nibble + 1U) & 0xFU);
            editValue_ &= ~(static_cast<uint64_t>(0xFU) << shift);
            editValue_ |= (static_cast<uint64_t>(nibble) << shift);
            needsRedraw_ = true;
        }
        else if (input == InputManager::InputEvent::DOWN)
        {
            // Decrement the nibble at current position
            const size_t shift = (HEX_NIBBLES - 1U - editNibbleIdx_) * 4U;
            uint8_t nibble = static_cast<uint8_t>((editValue_ >> shift) & 0xFU);
            nibble = static_cast<uint8_t>((nibble - 1U) & 0xFU);
            editValue_ &= ~(static_cast<uint64_t>(0xFU) << shift);
            editValue_ |= (static_cast<uint64_t>(nibble) << shift);
            needsRedraw_ = true;
        }
        else if (input == InputManager::InputEvent::RIGHT)
        {
            editNibbleIdx_ = (editNibbleIdx_ + 1U) % HEX_NIBBLES;
            needsRedraw_ = true;
        }
        else if (input == InputManager::InputEvent::LEFT)
        {
            if (editNibbleIdx_ == 0U)
            {
                // Exit editor on LEFT at first position
                transitionTo(IRState::MAIN_MENU);
                mainMenu_.setItems(IR_MENU_LABELS, IR_MENU_COUNT);
                return;
            }
            editNibbleIdx_ = editNibbleIdx_ - 1U;
            needsRedraw_ = true;
        }
        else if (input == InputManager::InputEvent::BUTTON_PRESS)
        {
            // Send the edited code
            IRTransceiver::instance().initTransmit();
            IRTransceiver::instance().send(editValue_, editProto_, editBits_);
            IRTransceiver::instance().deinit();
            ESP_LOGI(TAG_IR_APP, "Editor sent: proto=%d 0x%llX %ubits",
                     static_cast<int>(editProto_),
                     static_cast<unsigned long long>(editValue_),
                     static_cast<unsigned>(editBits_));
            needsRedraw_ = true;
        }
    }

    // ── DB Manager: Save ────────────────────────────────────────────────

    void enterDbSave()
    {
        saveNameLen_ = 0U;
        std::memset(saveName_, 0, sizeof(saveName_));
        saveNameCursorChar_ = 'A';
        transitionTo(IRState::DB_SAVE);
    }

    void drawDbSaveView()
    {
        if (!IRTransceiver::instance().hasLastCode())
        {
            DisplayManager::instance().drawText(2, 28, "No code captured.");
            DisplayManager::instance().drawText(2, 40, "Use Sniffer first.");
            return;
        }

        DisplayManager::instance().drawText(2, 24, "Name:");

        // Show current name being built
        char nameBuf[24];
        std::snprintf(nameBuf, sizeof(nameBuf), "%.14s%c_",
                      saveName_, saveNameCursorChar_);
        DisplayManager::instance().drawText(2, 34, nameBuf);

        DisplayManager::instance().drawText(2, 46, "U/D:char R:next OK:save");

        char codeBuf[24];
        std::snprintf(codeBuf, sizeof(codeBuf), "Code: 0x%08llX",
                      static_cast<unsigned long long>(
                          IRTransceiver::instance().lastValue()));
        DisplayManager::instance().drawText(2, 58, codeBuf);
    }

    void dbSaveHandleInput(InputManager::InputEvent input)
    {
        if (input == InputManager::InputEvent::UP)
        {
            // Cycle character up: A-Z, 0-9, _
            if (saveNameCursorChar_ >= 'A' && saveNameCursorChar_ < 'Z')
            {
                ++saveNameCursorChar_;
            }
            else if (saveNameCursorChar_ == 'Z')
            {
                saveNameCursorChar_ = '0';
            }
            else if (saveNameCursorChar_ >= '0' && saveNameCursorChar_ < '9')
            {
                ++saveNameCursorChar_;
            }
            else if (saveNameCursorChar_ == '9')
            {
                saveNameCursorChar_ = '_';
            }
            else
            {
                saveNameCursorChar_ = 'A';
            }
            needsRedraw_ = true;
        }
        else if (input == InputManager::InputEvent::DOWN)
        {
            // Cycle character down
            if (saveNameCursorChar_ > 'A' && saveNameCursorChar_ <= 'Z')
            {
                --saveNameCursorChar_;
            }
            else if (saveNameCursorChar_ == 'A')
            {
                saveNameCursorChar_ = '_';
            }
            else if (saveNameCursorChar_ == '_')
            {
                saveNameCursorChar_ = '9';
            }
            else if (saveNameCursorChar_ > '0' && saveNameCursorChar_ <= '9')
            {
                --saveNameCursorChar_;
            }
            else
            {
                saveNameCursorChar_ = 'Z';
            }
            needsRedraw_ = true;
        }
        else if (input == InputManager::InputEvent::RIGHT)
        {
            // Accept current character and advance
            if (saveNameLen_ < sizeof(saveName_) - 2U)
            {
                saveName_[saveNameLen_] = saveNameCursorChar_;
                ++saveNameLen_;
                saveName_[saveNameLen_] = '\0';
                saveNameCursorChar_ = 'A';
            }
            needsRedraw_ = true;
        }
        else if (input == InputManager::InputEvent::LEFT)
        {
            if (saveNameLen_ > 0U)
            {
                --saveNameLen_;
                saveName_[saveNameLen_] = '\0';
            }
            else
            {
                transitionTo(IRState::MAIN_MENU);
                mainMenu_.setItems(IR_MENU_LABELS, IR_MENU_COUNT);
            }
            needsRedraw_ = true;
        }
        else if (input == InputManager::InputEvent::BUTTON_PRESS)
        {
            // Finalise the name: append the current cursor char
            if (saveNameLen_ < sizeof(saveName_) - 2U)
            {
                saveName_[saveNameLen_] = saveNameCursorChar_;
                ++saveNameLen_;
                saveName_[saveNameLen_] = '\0';
            }
            performDbSave();
            transitionTo(IRState::MAIN_MENU);
            mainMenu_.setItems(IR_MENU_LABELS, IR_MENU_COUNT);
        }
    }

    void performDbSave()
    {
        if (!IRTransceiver::instance().hasLastCode() || saveNameLen_ == 0U)
        {
            return;
        }

        // Build file path: /ext/assets/ir/saved/<name>.csv
        char path[64];
        std::snprintf(path, sizeof(path), "%s/%s.csv", IR_SAVED_DIR, saveName_);

        auto &vfs = hackos::storage::VirtualFS::instance();
        fs::File f = vfs.open(path, "w");
        if (!f)
        {
            ESP_LOGE(TAG_IR_APP, "Cannot open %s for writing", path);
            return;
        }

        const String &protoStr = typeToString(
            IRTransceiver::instance().lastProtocol(), false);

        f.printf("# name,protocol_id,hex_code,bits\n");
        f.printf("%s,%d,0x%llX,%u\n",
                 saveName_,
                 static_cast<int>(IRTransceiver::instance().lastProtocol()),
                 static_cast<unsigned long long>(IRTransceiver::instance().lastValue()),
                 static_cast<unsigned>(IRTransceiver::instance().lastBits()));

        f.close();
        ESP_LOGI(TAG_IR_APP, "Saved code to %s (proto=%s)", path, protoStr.c_str());
    }

    // ── DB Manager: Load ────────────────────────────────────────────────

    void enterDbLoad()
    {
        auto &vfs = hackos::storage::VirtualFS::instance();
        hackos::storage::VirtualFS::DirEntry entries[DB_MAX_ENTRIES];
        const size_t count = vfs.listDir(IR_SAVED_DIR, entries, DB_MAX_ENTRIES);

        dbEntryCount_ = 0U;
        for (size_t i = 0U; i < count && dbEntryCount_ < DB_MAX_ENTRIES; ++i)
        {
            if (entries[i].isDir)
            {
                continue;
            }
            // Copy name (strip .csv extension for display)
            std::snprintf(dbEntryNames_[dbEntryCount_],
                          sizeof(dbEntryNames_[0]),
                          "%.19s", entries[i].name);
            char *dot = std::strrchr(dbEntryNames_[dbEntryCount_], '.');
            if (dot != nullptr)
            {
                *dot = '\0';
            }
            dbMenuLabels_[dbEntryCount_] = dbEntryNames_[dbEntryCount_];
            ++dbEntryCount_;
        }

        if (dbEntryCount_ == 0U)
        {
            ESP_LOGW(TAG_IR_APP, "No saved IR codes found in %s", IR_SAVED_DIR);
            return;
        }

        dbMenu_.setItems(dbMenuLabels_, dbEntryCount_);
        transitionTo(IRState::DB_LOAD_LIST);
    }

    void performDbLoad(size_t index)
    {
        if (index >= dbEntryCount_)
        {
            return;
        }

        char path[64];
        std::snprintf(path, sizeof(path), "%s/%s.csv",
                      IR_SAVED_DIR, dbEntryNames_[index]);

        auto &vfs = hackos::storage::VirtualFS::instance();
        fs::File f = vfs.open(path, "r");
        if (!f)
        {
            ESP_LOGE(TAG_IR_APP, "Cannot open %s for reading", path);
            return;
        }

        char line[CSV_LINE_MAX];
        while (f.available())
        {
            const size_t len = f.readBytesUntil('\n', line, sizeof(line) - 1U);
            if (len == 0U)
            {
                continue;
            }
            line[len] = '\0';

            if (line[0] == '#')
            {
                continue;
            }

            // Parse: name,protocol_id,hex_code,bits
            char *saveptr = nullptr;
            (void)strtok_r(line, ",", &saveptr); // skip name
            char *protoStr = strtok_r(nullptr, ",", &saveptr);
            char *codeStr = strtok_r(nullptr, ",", &saveptr);
            char *bitsStr = strtok_r(nullptr, ",\r\n", &saveptr);

            if (protoStr != nullptr && codeStr != nullptr && bitsStr != nullptr)
            {
                editProto_ = static_cast<decode_type_t>(
                    std::strtol(protoStr, nullptr, 10));
                editValue_ = std::strtoull(codeStr, nullptr, 16);
                editBits_ = static_cast<uint16_t>(
                    std::strtoul(bitsStr, nullptr, 10));

                const String &pStr = typeToString(editProto_, false);
                std::snprintf(protoName_, sizeof(protoName_), "%.15s", pStr.c_str());
                std::snprintf(codeHex_, sizeof(codeHex_), "0x%08llX",
                              static_cast<unsigned long long>(editValue_));

                ESP_LOGI(TAG_IR_APP, "Loaded: %s 0x%llX %ubits from %s",
                         protoName_,
                         static_cast<unsigned long long>(editValue_),
                         static_cast<unsigned>(editBits_),
                         path);
            }
            break; // Only read the first data line
        }

        f.close();
        transitionTo(IRState::DB_LOADED);
    }

    void drawDbLoadedView()
    {
        DisplayManager::instance().drawText(2, 24, protoName_);
        DisplayManager::instance().drawText(2, 36, codeHex_);

        char bits[16];
        std::snprintf(bits, sizeof(bits), "Bits: %u",
                      static_cast<unsigned>(editBits_));
        DisplayManager::instance().drawText(2, 48, bits);
        DisplayManager::instance().drawText(2, 58, "OK:send  L:back");
    }

    // ── Save IR code (legacy format) ────────────────────────────────────

    void saveIrCodeToSd()
    {
        if (!IRTransceiver::instance().hasLastCode())
        {
            ESP_LOGW(TAG_IR_APP, "saveIrCodeToSd: no code captured");
            return;
        }
        if (!StorageManager::instance().isMounted())
        {
            ESP_LOGW(TAG_IR_APP, "saveIrCodeToSd: SD not mounted");
            return;
        }

        char buf[64];
        const int len = std::snprintf(buf, sizeof(buf),
            "Proto: %s\nCode: %s\nBits: %u\n",
            protoName_,
            codeHex_,
            static_cast<unsigned>(IRTransceiver::instance().lastBits()));

        if (len > 0 && len < static_cast<int>(sizeof(buf)))
        {
            const bool ok = StorageManager::instance().appendChunk(
                "/captures/ir_codes.txt",
                reinterpret_cast<const uint8_t *>(buf),
                static_cast<size_t>(len));
            ESP_LOGI(TAG_IR_APP, "saveIrCodeToSd: %s", ok ? "OK" : "FAIL");
        }
    }

    // ── Input routing ───────────────────────────────────────────────────

    void handleInput(InputManager::InputEvent input)
    {
        switch (state_)
        {
        case IRState::MAIN_MENU:
            handleMainMenu(input);
            break;
        case IRState::SNIFFER:
        case IRState::CLONER:
            if (input == InputManager::InputEvent::BUTTON_PRESS ||
                input == InputManager::InputEvent::LEFT)
            {
                IRTransceiver::instance().deinit();
                transitionTo(IRState::MAIN_MENU);
                mainMenu_.setItems(IR_MENU_LABELS, IR_MENU_COUNT);
            }
            break;
        case IRState::TV_BGONE_RUNNING:
            if (input == InputManager::InputEvent::BUTTON_PRESS ||
                input == InputManager::InputEvent::LEFT)
            {
                tvbRunning_ = false;
                IRTransceiver::instance().deinit();
                transitionTo(IRState::MAIN_MENU);
                mainMenu_.setItems(IR_MENU_LABELS, IR_MENU_COUNT);
                ESP_LOGI(TAG_IR_APP, "TV-B-Gone cancelled");
            }
            break;
        case IRState::TV_BGONE_DONE:
            if (input == InputManager::InputEvent::BUTTON_PRESS ||
                input == InputManager::InputEvent::LEFT)
            {
                transitionTo(IRState::MAIN_MENU);
                mainMenu_.setItems(IR_MENU_LABELS, IR_MENU_COUNT);
            }
            break;
        case IRState::SIGNAL_EDITOR:
            editorHandleInput(input);
            break;
        case IRState::DB_SAVE:
            dbSaveHandleInput(input);
            break;
        case IRState::DB_LOAD_LIST:
            handleDbLoadList(input);
            break;
        case IRState::DB_LOADED:
            handleDbLoaded(input);
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
            switch (sel)
            {
            case 0U: // TV-B-Gone
                startTvBGone();
                break;
            case 1U: // IR Sniffer
                IRTransceiver::instance().initReceive();
                transitionTo(IRState::SNIFFER);
                break;
            case 2U: // IR Cloner
                IRTransceiver::instance().initTransmit();
                clonerSent_ = false;
                transitionTo(IRState::CLONER);
                break;
            case 3U: // Signal Editor
                enterEditor();
                break;
            case 4U: // Save to DB
                enterDbSave();
                break;
            case 5U: // Load from DB
                enterDbLoad();
                break;
            case 6U: // Back
            {
                const Event evt{EventType::EVT_SYSTEM, SYSTEM_EVENT_BACK,
                                0, nullptr};
                EventSystem::instance().postEvent(evt);
                break;
            }
            default:
                break;
            }
        }
    }

    void handleDbLoadList(InputManager::InputEvent input)
    {
        if (input == InputManager::InputEvent::UP)
        {
            dbMenu_.moveSelection(-1);
        }
        else if (input == InputManager::InputEvent::DOWN)
        {
            dbMenu_.moveSelection(1);
        }
        else if (input == InputManager::InputEvent::BUTTON_PRESS)
        {
            performDbLoad(dbMenu_.selectedIndex());
        }
        else if (input == InputManager::InputEvent::LEFT)
        {
            transitionTo(IRState::MAIN_MENU);
            mainMenu_.setItems(IR_MENU_LABELS, IR_MENU_COUNT);
        }
    }

    void handleDbLoaded(InputManager::InputEvent input)
    {
        if (input == InputManager::InputEvent::BUTTON_PRESS)
        {
            // Send the loaded code
            IRTransceiver::instance().initTransmit();
            IRTransceiver::instance().send(editValue_, editProto_, editBits_);
            IRTransceiver::instance().deinit();
            ESP_LOGI(TAG_IR_APP, "Sent loaded code: 0x%llX",
                     static_cast<unsigned long long>(editValue_));
        }
        else if (input == InputManager::InputEvent::LEFT)
        {
            transitionTo(IRState::MAIN_MENU);
            mainMenu_.setItems(IR_MENU_LABELS, IR_MENU_COUNT);
        }
    }
};

} // namespace

AppBase *createIRToolsApp()
{
    return new (std::nothrow) IRToolsApp();
}
