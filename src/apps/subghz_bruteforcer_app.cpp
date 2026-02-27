/**
 * @file subghz_bruteforcer_app.cpp
 * @brief Phase 13 – Sub-GHz Bruteforcer for fixed-code protocols.
 *
 * Iterates through all code combinations for fixed-code 433 MHz protocols
 * (PT2262/SC2262 "Princeton", CAME 12-bit, Nice FLO) and transmits each
 * code 3 times via OOK modulation on the TX pin (GPIO25).
 *
 * Features:
 *  - **Protocol Generator**: Precise PWM pulse generation for PT2262, CAME
 *    and Nice FLO encodings at configurable bit lengths (10, 12, 24).
 *  - **Bruteforce Modes**: Binary (PT2262), CAME, Nice FLO.
 *  - **Fast-Transmit**: Each code sent 3 times before advancing.
 *  - **Live UI**: Shows current hex code, progress, and estimated remaining
 *    time in MM:SS format.
 */

#include "apps/subghz_bruteforcer_app.h"

#include <Arduino.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <new>

#include "config.h"
#include "core/event.h"
#include "core/event_system.h"
#include "hardware/display.h"
#include "hardware/input.h"
#include "ui/widgets.h"

static constexpr const char *TAG_BF = "SubGhzBF";

// ── Hardware constants ──────────────────────────────────────────────────────

static constexpr gpio_num_t RF_TX_GPIO = static_cast<gpio_num_t>(PIN_RF_TX);

// ── Protocol timing constants (microseconds) ────────────────────────────────

/// PT2262 / SC2262 "Princeton" protocol timings.
static constexpr uint16_t PT2262_SHORT_US  = 350U;
static constexpr uint16_t PT2262_LONG_US   = 1050U;  ///< 3 × short
static constexpr uint16_t PT2262_SYNC_LOW  = 10850U;  ///< 31 × short

/// CAME 12-bit protocol timings.
static constexpr uint16_t CAME_SHORT_US    = 320U;
static constexpr uint16_t CAME_LONG_US     = 640U;    ///< 2 × short
static constexpr uint16_t CAME_SYNC_LOW    = 9920U;   ///< 31 × short

/// Nice FLO protocol timings.
static constexpr uint16_t NICE_SHORT_US    = 700U;
static constexpr uint16_t NICE_LONG_US     = 1400U;   ///< 2 × short
static constexpr uint16_t NICE_SYNC_LOW    = 25200U;  ///< 36 × short

/// Number of times to transmit each code for reliable reception.
static constexpr uint8_t TX_REPEAT_COUNT = 3U;

/// Inter-code gap (µs) between repetitions of the same code.
static constexpr uint16_t INTER_CODE_GAP_US = 5000U;

// ── Maximum raw timing buffer ───────────────────────────────────────────────

/// Worst-case: 24-bit code × 4 timing edges/bit + 2 sync edges = 98 entries.
/// Round up generously.
static constexpr size_t MAX_TIMINGS = 128U;

namespace
{

// ── Protocol definitions ────────────────────────────────────────────────────

enum class Protocol : uint8_t
{
    PT2262 = 0U,
    CAME   = 1U,
    NICE   = 2U,
    COUNT  = 3U,
};

static const char *const PROTOCOL_NAMES[] = {
    "PT2262 (Binary)",
    "CAME 12-bit",
    "Nice FLO",
};

static const uint8_t PROTOCOL_DEFAULT_BITS[] = {
    12U, // PT2262 default
    12U, // CAME always 12
    12U, // Nice always 12
};

/// Allowed bit-length options for PT2262 (Binary) mode.
static constexpr size_t BIT_OPTION_COUNT = 3U;
static const uint8_t BIT_OPTIONS[] = {10U, 12U, 24U};
static const char *const BIT_LABELS[] = {"10 bits", "12 bits", "24 bits"};

// ── Pulse encoder functions ─────────────────────────────────────────────────

/**
 * @brief Encode a code word using PT2262/SC2262 (Princeton) protocol.
 *
 * Encoding per bit:
 *   '0': SHORT high, LONG low, SHORT high, LONG low
 *   '1': LONG high, SHORT low, LONG high, SHORT low
 *
 * Sync pulse appended at the end: SHORT high, SYNC_LOW low.
 *
 * @param code      The numeric code to encode.
 * @param bitCount  Number of bits (10, 12, or 24).
 * @param timings   Output buffer for ±microsecond timings (+ = HIGH, − = LOW).
 * @param maxLen    Maximum entries in the timings buffer.
 * @return          Number of timing entries written.
 */
static size_t encodePT2262(uint32_t code, uint8_t bitCount,
                           int32_t *timings, size_t maxLen)
{
    size_t idx = 0U;

    for (int8_t bit = static_cast<int8_t>(bitCount) - 1; bit >= 0 && (idx + 4U) <= maxLen; --bit)
    {
        const bool isOne = ((code >> bit) & 1U) != 0U;
        if (isOne)
        {
            timings[idx++] = static_cast<int32_t>(PT2262_LONG_US);    // HIGH
            timings[idx++] = -static_cast<int32_t>(PT2262_SHORT_US);  // LOW
            timings[idx++] = static_cast<int32_t>(PT2262_LONG_US);    // HIGH
            timings[idx++] = -static_cast<int32_t>(PT2262_SHORT_US);  // LOW
        }
        else
        {
            timings[idx++] = static_cast<int32_t>(PT2262_SHORT_US);   // HIGH
            timings[idx++] = -static_cast<int32_t>(PT2262_LONG_US);   // LOW
            timings[idx++] = static_cast<int32_t>(PT2262_SHORT_US);   // HIGH
            timings[idx++] = -static_cast<int32_t>(PT2262_LONG_US);   // LOW
        }
    }

    // Sync pulse: SHORT high + long sync low
    if ((idx + 2U) <= maxLen)
    {
        timings[idx++] = static_cast<int32_t>(PT2262_SHORT_US);
        timings[idx++] = -static_cast<int32_t>(PT2262_SYNC_LOW);
    }

    return idx;
}

/**
 * @brief Encode a code word using CAME 12-bit protocol.
 *
 * Encoding per bit:
 *   '0': SHORT high, LONG low
 *   '1': LONG high, SHORT low
 *
 * Sync pulse prepended: SHORT high, SYNC_LOW low.
 */
static size_t encodeCame(uint32_t code, uint8_t bitCount,
                         int32_t *timings, size_t maxLen)
{
    size_t idx = 0U;

    // Sync: SHORT high, SYNC_LOW low
    if ((idx + 2U) <= maxLen)
    {
        timings[idx++] = static_cast<int32_t>(CAME_SHORT_US);
        timings[idx++] = -static_cast<int32_t>(CAME_SYNC_LOW);
    }

    for (int8_t bit = static_cast<int8_t>(bitCount) - 1; bit >= 0 && (idx + 2U) <= maxLen; --bit)
    {
        const bool isOne = ((code >> bit) & 1U) != 0U;
        if (isOne)
        {
            timings[idx++] = static_cast<int32_t>(CAME_LONG_US);     // HIGH
            timings[idx++] = -static_cast<int32_t>(CAME_SHORT_US);   // LOW
        }
        else
        {
            timings[idx++] = static_cast<int32_t>(CAME_SHORT_US);    // HIGH
            timings[idx++] = -static_cast<int32_t>(CAME_LONG_US);    // LOW
        }
    }

    return idx;
}

/**
 * @brief Encode a code word using Nice FLO protocol.
 *
 * Encoding per bit:
 *   '0': SHORT high, LONG low
 *   '1': LONG high, SHORT low
 *
 * Sync pulse prepended: SHORT high, SYNC_LOW low.
 */
static size_t encodeNice(uint32_t code, uint8_t bitCount,
                         int32_t *timings, size_t maxLen)
{
    size_t idx = 0U;

    // Sync: SHORT high, SYNC_LOW low
    if ((idx + 2U) <= maxLen)
    {
        timings[idx++] = static_cast<int32_t>(NICE_SHORT_US);
        timings[idx++] = -static_cast<int32_t>(NICE_SYNC_LOW);
    }

    for (int8_t bit = static_cast<int8_t>(bitCount) - 1; bit >= 0 && (idx + 2U) <= maxLen; --bit)
    {
        const bool isOne = ((code >> bit) & 1U) != 0U;
        if (isOne)
        {
            timings[idx++] = static_cast<int32_t>(NICE_LONG_US);     // HIGH
            timings[idx++] = -static_cast<int32_t>(NICE_SHORT_US);   // LOW
        }
        else
        {
            timings[idx++] = static_cast<int32_t>(NICE_SHORT_US);    // HIGH
            timings[idx++] = -static_cast<int32_t>(NICE_LONG_US);    // LOW
        }
    }

    return idx;
}

// ── App states ──────────────────────────────────────────────────────────────

enum class BFState : uint8_t
{
    MAIN_MENU,       ///< Select protocol
    SELECT_BITS,     ///< Select bit length (PT2262 only)
    CONFIRM,         ///< Show summary, press START
    RUNNING,         ///< Bruteforce in progress
    PAUSED,          ///< Bruteforce paused
    DONE,            ///< All codes transmitted
};

// ── Menu definitions ────────────────────────────────────────────────────────

static constexpr size_t MAIN_MENU_COUNT = 4U;
static const char *const MAIN_MENU_LABELS[MAIN_MENU_COUNT] = {
    "PT2262 (Binary)",
    "CAME 12-bit",
    "Nice FLO",
    "Back",
};

// ═════════════════════════════════════════════════════════════════════════════
// ── SubGhzBruteforceApp ─────────────────────────────────────────────────────
// ═════════════════════════════════════════════════════════════════════════════

class SubGhzBruteforceApp final : public AppBase, public IEventObserver
{
public:
    SubGhzBruteforceApp()
        : statusBar_(0, 0, 128, 8),
          mainMenu_(0, 20, 128, 36, 3),
          bitsMenu_(0, 20, 128, 36, 3),
          state_(BFState::MAIN_MENU),
          needsRedraw_(true),
          protocol_(Protocol::PT2262),
          bitCount_(12U),
          totalCodes_(0UL),
          currentCode_(0UL),
          startTimeMs_(0UL),
          codeTimeUs_(0UL),
          txInitialised_(false),
          timingBuf_{}
    {
    }

    // ── AppBase lifecycle ────────────────────────────────────────────────

    void onSetup() override
    {
        statusBar_.setConnectivity(false, false);
        statusBar_.setBatteryLevel(100U);
        statusBar_.setTime(0U, 0U);
        mainMenu_.setItems(MAIN_MENU_LABELS, MAIN_MENU_COUNT);
        bitsMenu_.setItems(BIT_LABELS, BIT_OPTION_COUNT);
        (void)EventSystem::instance().subscribe(this);
        state_ = BFState::MAIN_MENU;
        needsRedraw_ = true;

        ESP_LOGI(TAG_BF, "setup complete");
    }

    void onLoop() override
    {
        if (state_ == BFState::RUNNING)
        {
            // Transmit next code batch (one code × 3 repeats per loop tick).
            if (currentCode_ < totalCodes_)
            {
                transmitCode(static_cast<uint32_t>(currentCode_));
                ++currentCode_;
                needsRedraw_ = true;
            }
            else
            {
                finishTx();
                state_ = BFState::DONE;
                needsRedraw_ = true;
            }
        }
    }

    void onDraw() override
    {
        if (!needsRedraw_ && !statusBar_.isDirty())
        {
            return;
        }

        DisplayManager::instance().clear();
        statusBar_.draw();

        switch (state_)
        {
        case BFState::MAIN_MENU:
            drawTitle("SubGHz Bruteforcer");
            mainMenu_.draw();
            break;

        case BFState::SELECT_BITS:
            drawTitle("Select Bit Length");
            bitsMenu_.draw();
            break;

        case BFState::CONFIRM:
            drawConfirmScreen();
            break;

        case BFState::RUNNING:
            drawRunningScreen();
            break;

        case BFState::PAUSED:
            drawPausedScreen();
            break;

        case BFState::DONE:
            drawDoneScreen();
            break;
        }

        DisplayManager::instance().present();
        statusBar_.clearDirty();
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
        finishTx();
        EventSystem::instance().unsubscribe(this);
        ESP_LOGI(TAG_BF, "destroyed");
    }

private:
    static constexpr size_t LINE_LEN = 32U;

    StatusBar statusBar_;
    MenuListView mainMenu_;
    MenuListView bitsMenu_;
    BFState state_;
    bool needsRedraw_;

    Protocol protocol_;
    uint8_t bitCount_;
    uint32_t totalCodes_;
    uint32_t currentCode_;
    uint32_t startTimeMs_;
    uint32_t codeTimeUs_;  ///< Estimated µs per single code transmission (×3).
    bool txInitialised_;

    int32_t timingBuf_[MAX_TIMINGS];

    // ── State transitions ────────────────────────────────────────────────

    void transitionTo(BFState next)
    {
        state_ = next;
        needsRedraw_ = true;
    }

    // ── Drawing helpers ──────────────────────────────────────────────────

    void drawTitle(const char *title)
    {
        DisplayManager::instance().drawText(2, 10, title);
        DisplayManager::instance().drawLine(0, 18, 127, 18);
    }

    void drawConfirmScreen()
    {
        drawTitle("Confirm Attack");

        char line[LINE_LEN];
        std::snprintf(line, sizeof(line), "Proto: %s",
                      PROTOCOL_NAMES[static_cast<uint8_t>(protocol_)]);
        DisplayManager::instance().drawText(2, 24, line);

        std::snprintf(line, sizeof(line), "Bits: %u  Codes: %lu",
                      bitCount_, static_cast<unsigned long>(totalCodes_));
        DisplayManager::instance().drawText(2, 34, line);

        // Estimated total time
        const uint32_t estTotalSec = estimateTotalSeconds();
        const uint32_t mm = estTotalSec / 60U;
        const uint32_t ss = estTotalSec % 60U;
        std::snprintf(line, sizeof(line), "Est. time: %02lu:%02lu",
                      static_cast<unsigned long>(mm),
                      static_cast<unsigned long>(ss));
        DisplayManager::instance().drawText(2, 44, line);

        DisplayManager::instance().drawText(2, 56, "CENTER=Start  LEFT=Back");
    }

    void drawRunningScreen()
    {
        drawTitle("Bruteforcing...");

        char line[LINE_LEN];

        // Current code in hex
        std::snprintf(line, sizeof(line), "Code: 0x%06lX",
                      static_cast<unsigned long>(currentCode_));
        DisplayManager::instance().drawText(2, 24, line);

        // Progress
        const uint32_t percent = (totalCodes_ > 0U)
                                     ? static_cast<uint32_t>(
                                           (static_cast<uint64_t>(currentCode_) * 100ULL) / totalCodes_)
                                     : 0U;
        std::snprintf(line, sizeof(line), "%lu/%lu (%lu%%)",
                      static_cast<unsigned long>(currentCode_),
                      static_cast<unsigned long>(totalCodes_),
                      static_cast<unsigned long>(percent));
        DisplayManager::instance().drawText(2, 34, line);

        // Remaining time estimate
        const uint32_t remaining = estimateRemainingSeconds();
        const uint32_t mm = remaining / 60U;
        const uint32_t ss = remaining % 60U;
        std::snprintf(line, sizeof(line), "Remaining: %02lu:%02lu",
                      static_cast<unsigned long>(mm),
                      static_cast<unsigned long>(ss));
        DisplayManager::instance().drawText(2, 44, line);

        DisplayManager::instance().drawText(2, 56, "CENTER=Pause  LEFT=Stop");
    }

    void drawPausedScreen()
    {
        drawTitle("PAUSED");

        char line[LINE_LEN];
        std::snprintf(line, sizeof(line), "Code: 0x%06lX",
                      static_cast<unsigned long>(currentCode_));
        DisplayManager::instance().drawText(2, 24, line);

        const uint32_t percent = (totalCodes_ > 0U)
                                     ? static_cast<uint32_t>(
                                           (static_cast<uint64_t>(currentCode_) * 100ULL) / totalCodes_)
                                     : 0U;
        std::snprintf(line, sizeof(line), "Progress: %lu%%",
                      static_cast<unsigned long>(percent));
        DisplayManager::instance().drawText(2, 36, line);

        DisplayManager::instance().drawText(2, 50, "CENTER=Resume LEFT=Stop");
    }

    void drawDoneScreen()
    {
        drawTitle("Complete");

        char line[LINE_LEN];
        std::snprintf(line, sizeof(line), "%lu codes sent",
                      static_cast<unsigned long>(totalCodes_));
        DisplayManager::instance().drawText(2, 30, line);

        const uint32_t elapsed = (millis() - startTimeMs_) / 1000U;
        const uint32_t mm = elapsed / 60U;
        const uint32_t ss = elapsed % 60U;
        std::snprintf(line, sizeof(line), "Time: %02lu:%02lu",
                      static_cast<unsigned long>(mm),
                      static_cast<unsigned long>(ss));
        DisplayManager::instance().drawText(2, 42, line);

        DisplayManager::instance().drawText(2, 56, "Press to return");
    }

    // ── Time estimation ──────────────────────────────────────────────────

    /**
     * @brief Estimate the duration (µs) of a single code transmission.
     *
     * Encodes a sample code (all zeros) to measure the total pulse duration,
     * then multiplies by TX_REPEAT_COUNT and adds inter-code gaps.
     */
    uint32_t estimateSingleCodeUs() const
    {
        int32_t tmpBuf[MAX_TIMINGS];
        const size_t count = encodeCode(0U, tmpBuf, MAX_TIMINGS);

        uint32_t totalUs = 0U;
        for (size_t i = 0U; i < count; ++i)
        {
            const int32_t t = tmpBuf[i];
            totalUs += static_cast<uint32_t>(t > 0 ? t : -t);
        }

        // 3 repeats + inter-code gaps
        return (totalUs * TX_REPEAT_COUNT) +
               (INTER_CODE_GAP_US * (TX_REPEAT_COUNT - 1U));
    }

    uint32_t estimateTotalSeconds() const
    {
        const uint64_t totalUs = static_cast<uint64_t>(codeTimeUs_) *
                                 static_cast<uint64_t>(totalCodes_);
        return static_cast<uint32_t>(totalUs / 1000000ULL);
    }

    uint32_t estimateRemainingSeconds() const
    {
        if (currentCode_ == 0U)
        {
            return estimateTotalSeconds();
        }

        // Use actual elapsed time to refine the estimate.
        const uint32_t elapsedMs = millis() - startTimeMs_;
        if (elapsedMs == 0U || currentCode_ == 0U)
        {
            return estimateTotalSeconds();
        }

        const uint32_t msPerCode = elapsedMs / static_cast<uint32_t>(currentCode_);
        const uint32_t remainingCodes = totalCodes_ - static_cast<uint32_t>(currentCode_);
        return (static_cast<uint64_t>(msPerCode) * remainingCodes) / 1000ULL;
    }

    // ── Protocol encoding dispatch ───────────────────────────────────────

    size_t encodeCode(uint32_t code, int32_t *buf, size_t maxLen) const
    {
        switch (protocol_)
        {
        case Protocol::CAME:
            return encodeCame(code, bitCount_, buf, maxLen);
        case Protocol::NICE:
            return encodeNice(code, bitCount_, buf, maxLen);
        case Protocol::PT2262:
        default:
            return encodePT2262(code, bitCount_, buf, maxLen);
        }
    }

    // ── TX control ───────────────────────────────────────────────────────

    void initTx()
    {
        if (txInitialised_)
        {
            return;
        }

        gpio_config_t io_conf = {};
        io_conf.pin_bit_mask = (1ULL << RF_TX_GPIO);
        io_conf.mode         = GPIO_MODE_OUTPUT;
        io_conf.pull_up_en   = GPIO_PULLUP_DISABLE;
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.intr_type    = GPIO_INTR_DISABLE;
        gpio_config(&io_conf);
        gpio_set_level(RF_TX_GPIO, 0);

        txInitialised_ = true;
        ESP_LOGI(TAG_BF, "TX initialised on GPIO%u",
                 static_cast<unsigned>(PIN_RF_TX));
    }

    void finishTx()
    {
        if (txInitialised_)
        {
            gpio_set_level(RF_TX_GPIO, 0);
            txInitialised_ = false;
        }
    }

    /**
     * @brief Transmit a single code word TX_REPEAT_COUNT times.
     *
     * Each repetition replays the exact encoded pulse timings on the GPIO
     * pin, with a short gap between repetitions.
     */
    void transmitCode(uint32_t code)
    {
        const size_t timingCount = encodeCode(code, timingBuf_, MAX_TIMINGS);
        if (timingCount == 0U)
        {
            return;
        }

        for (uint8_t rep = 0U; rep < TX_REPEAT_COUNT; ++rep)
        {
            for (size_t i = 0U; i < timingCount; ++i)
            {
                const int32_t timing = timingBuf_[i];
                const bool level = (timing > 0);
                const uint32_t duration = static_cast<uint32_t>(
                    timing > 0 ? timing : -timing);

                gpio_set_level(RF_TX_GPIO, level ? 1 : 0);
                delayMicroseconds(duration);
            }

            // Ensure TX pin is LOW between repetitions.
            gpio_set_level(RF_TX_GPIO, 0);
            if (rep < (TX_REPEAT_COUNT - 1U))
            {
                delayMicroseconds(INTER_CODE_GAP_US);
            }
        }
    }

    // ── Bruteforce control ───────────────────────────────────────────────

    void startBruteforce()
    {
        totalCodes_ = 1UL << bitCount_;
        currentCode_ = 0UL;
        codeTimeUs_ = estimateSingleCodeUs();
        startTimeMs_ = millis();

        initTx();
        transitionTo(BFState::RUNNING);

        ESP_LOGI(TAG_BF, "Starting bruteforce: proto=%s bits=%u codes=%lu",
                 PROTOCOL_NAMES[static_cast<uint8_t>(protocol_)],
                 bitCount_, static_cast<unsigned long>(totalCodes_));
    }

    // ── Input routing ────────────────────────────────────────────────────

    void handleInput(InputManager::InputEvent input)
    {
        switch (state_)
        {
        case BFState::MAIN_MENU:
            handleMainMenu(input);
            break;

        case BFState::SELECT_BITS:
            handleSelectBits(input);
            break;

        case BFState::CONFIRM:
            handleConfirm(input);
            break;

        case BFState::RUNNING:
            handleRunning(input);
            break;

        case BFState::PAUSED:
            handlePaused(input);
            break;

        case BFState::DONE:
            if (input == InputManager::InputEvent::BUTTON_PRESS ||
                input == InputManager::InputEvent::LEFT)
            {
                finishTx();
                transitionTo(BFState::MAIN_MENU);
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
            if (sel < static_cast<size_t>(Protocol::COUNT))
            {
                protocol_ = static_cast<Protocol>(sel);
                bitCount_ = PROTOCOL_DEFAULT_BITS[sel];

                if (protocol_ == Protocol::PT2262)
                {
                    // PT2262 supports variable bit lengths
                    bitsMenu_.setItems(BIT_LABELS, BIT_OPTION_COUNT);
                    transitionTo(BFState::SELECT_BITS);
                }
                else
                {
                    // CAME and Nice are fixed at 12 bits
                    transitionTo(BFState::CONFIRM);
                }
            }
            else if (sel == 3U)
            {
                // Back
                const Event evt{EventType::EVT_SYSTEM, SYSTEM_EVENT_BACK,
                                0, nullptr};
                EventSystem::instance().postEvent(evt);
            }
        }
        else if (input == InputManager::InputEvent::LEFT)
        {
            const Event evt{EventType::EVT_SYSTEM, SYSTEM_EVENT_BACK,
                            0, nullptr};
            EventSystem::instance().postEvent(evt);
        }
    }

    void handleSelectBits(InputManager::InputEvent input)
    {
        if (input == InputManager::InputEvent::UP)
        {
            bitsMenu_.moveSelection(-1);
        }
        else if (input == InputManager::InputEvent::DOWN)
        {
            bitsMenu_.moveSelection(1);
        }
        else if (input == InputManager::InputEvent::BUTTON_PRESS)
        {
            const size_t sel = bitsMenu_.selectedIndex();
            if (sel < BIT_OPTION_COUNT)
            {
                bitCount_ = BIT_OPTIONS[sel];
                transitionTo(BFState::CONFIRM);
            }
        }
        else if (input == InputManager::InputEvent::LEFT)
        {
            transitionTo(BFState::MAIN_MENU);
            mainMenu_.setItems(MAIN_MENU_LABELS, MAIN_MENU_COUNT);
        }
    }

    void handleConfirm(InputManager::InputEvent input)
    {
        if (input == InputManager::InputEvent::BUTTON_PRESS)
        {
            startBruteforce();
        }
        else if (input == InputManager::InputEvent::LEFT)
        {
            if (protocol_ == Protocol::PT2262)
            {
                transitionTo(BFState::SELECT_BITS);
                bitsMenu_.setItems(BIT_LABELS, BIT_OPTION_COUNT);
            }
            else
            {
                transitionTo(BFState::MAIN_MENU);
                mainMenu_.setItems(MAIN_MENU_LABELS, MAIN_MENU_COUNT);
            }
        }
    }

    void handleRunning(InputManager::InputEvent input)
    {
        if (input == InputManager::InputEvent::BUTTON_PRESS)
        {
            // Pause
            transitionTo(BFState::PAUSED);
        }
        else if (input == InputManager::InputEvent::LEFT)
        {
            // Stop and return to menu
            finishTx();
            transitionTo(BFState::MAIN_MENU);
            mainMenu_.setItems(MAIN_MENU_LABELS, MAIN_MENU_COUNT);
        }
    }

    void handlePaused(InputManager::InputEvent input)
    {
        if (input == InputManager::InputEvent::BUTTON_PRESS)
        {
            // Resume
            transitionTo(BFState::RUNNING);
        }
        else if (input == InputManager::InputEvent::LEFT)
        {
            // Stop and return to menu
            finishTx();
            transitionTo(BFState::MAIN_MENU);
            mainMenu_.setItems(MAIN_MENU_LABELS, MAIN_MENU_COUNT);
        }
    }
};

} // namespace

// ── Factory function ─────────────────────────────────────────────────────────

AppBase *createSubGhzBruteforceApp()
{
    return new (std::nothrow) SubGhzBruteforceApp();
}
