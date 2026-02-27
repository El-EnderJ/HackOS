/**
 * @file rf_tools_app.cpp
 * @brief Phase 9 – 433 MHz RF Tools: RAW Capture, Jammer, Save/Replay.
 *
 * Implements a low-level 433 MHz signal analyser without high-level RF
 * libraries.  All signal capture is done via a GPIO edge interrupt on the
 * RX pin (GPIO16), measuring pulse durations in microseconds with
 * esp_timer_get_time().
 *
 * Features:
 *  - **RAW Capture (Signal Sniffer)**: ISR-based pulse timing capture with
 *    a live mini-oscilloscope waveform on the OLED.
 *  - **Signal Jammer**: High-frequency PWM (LEDC) on the TX pin (GPIO25)
 *    to saturate 433.92 MHz.
 *  - **Save**: Exports captured timings to a Flipper-compatible `.sub` file
 *    on the SD card (`/ext/captures/rf_capture.sub`).
 *  - **Load & Replay**: Reads a `.sub` file and reproduces the exact pulse
 *    timings on the TX pin (Replay Attack).
 */

#include "apps/rf_tools_app.h"

#include <Arduino.h>
#include <cstdio>
#include <cstdlib>
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
#include "storage/vfs.h"
#include "ui/widgets.h"

static constexpr const char *TAG_RF_APP = "RFToolsApp";

// ── Constants ────────────────────────────────────────────────────────────────

/// Maximum raw timing samples stored per capture.
static constexpr size_t RAW_BUF_CAPACITY = 512U;

/// RX/TX GPIO numbers cast to the ESP-IDF enum type.
static constexpr gpio_num_t RF_RX_GPIO = static_cast<gpio_num_t>(PIN_RF_RX);
static constexpr gpio_num_t RF_TX_GPIO = static_cast<gpio_num_t>(PIN_RF_TX);

/// Carrier frequency written into .sub files (433.92 MHz).
static constexpr uint32_t RF_FREQUENCY_HZ = 433920000U;

/// LEDC channel and frequency used by the jammer.
static constexpr uint8_t LEDC_JAMMER_CHANNEL = 0U;
static constexpr uint32_t JAMMER_FREQ_HZ = 500000U;

/// Path for saved captures on the SD card.
static constexpr const char *CAPTURE_FILE_PATH = "/ext/captures/rf_capture.sub";

// ── ISR-shared state (kept in DRAM for IRAM ISR access) ─────────────────────

static volatile int32_t  s_rawTimings[RAW_BUF_CAPACITY];
static volatile uint16_t s_rawCount   = 0U;
static volatile int64_t  s_lastEdgeUs = 0;
static volatile bool     s_isrActive  = false;

/**
 * @brief GPIO edge ISR – measures pulse durations in microseconds.
 *
 * Fires on every edge of the RF RX pin.  The elapsed time since the
 * previous edge is recorded as a signed value: positive = mark (the
 * previous state was HIGH), negative = space (the previous state was LOW).
 *
 * This convention matches the Flipper SubGhz RAW format.
 */
static void IRAM_ATTR rfRxEdgeISR(void * /*arg*/)
{
    const int64_t nowUs = esp_timer_get_time();
    const int level = gpio_get_level(RF_RX_GPIO);

    if (s_lastEdgeUs == 0)
    {
        // First edge – just seed the timestamp.
        s_lastEdgeUs = nowUs;
        return;
    }

    int32_t duration = static_cast<int32_t>(nowUs - s_lastEdgeUs);
    s_lastEdgeUs = nowUs;

    // Clamp unreasonably long gaps.
    if (duration > 100000)
    {
        duration = 100000;
    }

    // After this edge the pin is at `level`.
    // The duration we measured belongs to the PREVIOUS state:
    //   pin now HIGH → previous was LOW → space → store negative
    //   pin now LOW  → previous was HIGH → mark  → store positive
    if (level)
    {
        duration = -duration;
    }

    if (s_rawCount < RAW_BUF_CAPACITY)
    {
        s_rawTimings[s_rawCount] = duration;
        ++s_rawCount;
    }
}

// ── Anonymous namespace for the app implementation ──────────────────────────

namespace
{

// ── App states ──────────────────────────────────────────────────────────────

enum class RFState : uint8_t
{
    MAIN_MENU,
    RAW_CAPTURE,
    SIGNAL_VIEW,
    JAMMING,
    SAVING,
    REPLAYING,
};

static constexpr size_t RF_MENU_COUNT = 6U;
static const char *const RF_MENU_LABELS[RF_MENU_COUNT] = {
    "RAW Capture",
    "Jammer",
    "Save Signal",
    "Replay Signal",
    "Load Signal",
    "Back",
};

// ── Waveform visualiser constants ───────────────────────────────────────────

static constexpr int16_t WAVE_X0     = 1;
static constexpr int16_t WAVE_X1     = 126;
static constexpr int16_t WAVE_Y_HIGH = 24;
static constexpr int16_t WAVE_Y_LOW  = 48;
static constexpr int16_t WAVE_WIDTH  = WAVE_X1 - WAVE_X0;

/// Maximum timing values per RAW_Data line in the .sub file.
static constexpr size_t SUB_VALUES_PER_LINE = 20U;

// ═════════════════════════════════════════════════════════════════════════════
// ── RFToolsApp ──────────────────────────────────────────────────────────────
// ═════════════════════════════════════════════════════════════════════════════

class RFToolsApp final : public AppBase, public IEventObserver
{
public:
    RFToolsApp()
        : statusBar_(0, 0, 128, 8),
          mainMenu_(0, 20, 128, 36, 3),
          state_(RFState::MAIN_MENU),
          needsRedraw_(true),
          jammerActive_(false),
          capturedCount_(0U),
          statusLine_{}
    {
        std::memset(capturedTimings_, 0, sizeof(capturedTimings_));
    }

    // ── AppBase lifecycle ────────────────────────────────────────────────

    void onSetup() override
    {
        statusBar_.setConnectivity(false, false);
        statusBar_.setBatteryLevel(100U);
        statusBar_.setTime(0U, 0U);
        mainMenu_.setItems(RF_MENU_LABELS, RF_MENU_COUNT);
        (void)EventSystem::instance().subscribe(this);
        state_ = RFState::MAIN_MENU;
        needsRedraw_ = true;

        // Install the GPIO ISR service (safe if already installed).
        const esp_err_t err = gpio_install_isr_service(0);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
        {
            ESP_LOGE(TAG_RF_APP, "gpio_install_isr_service failed: %d", err);
        }

        ESP_LOGI(TAG_RF_APP, "setup complete");
    }

    void onLoop() override
    {
        if (state_ == RFState::RAW_CAPTURE)
        {
            const uint16_t isrCount = s_rawCount;
            if (isrCount != capturedCount_)
            {
                copyIsrBuffer(isrCount);
                needsRedraw_ = true;
            }
        }
    }

    void onDraw() override
    {
        if (!needsRedraw_ && !statusBar_.isDirty() && !mainMenu_.isDirty())
        {
            return;
        }

        DisplayManager::instance().clear();
        statusBar_.draw();

        switch (state_)
        {
        case RFState::MAIN_MENU:
            drawTitle("RF 433MHz Tools");
            mainMenu_.draw();
            break;
        case RFState::RAW_CAPTURE:
            drawTitle("RAW Capture");
            drawCaptureView();
            break;
        case RFState::SIGNAL_VIEW:
            drawTitle("Signal View");
            drawWaveform();
            break;
        case RFState::JAMMING:
            drawTitle("Jammer Active");
            drawJammerView();
            break;
        case RFState::SAVING:
        case RFState::REPLAYING:
            drawTitle(state_ == RFState::SAVING ? "Save Signal" : "Replay");
            drawStatusView();
            break;
        }

        DisplayManager::instance().present();
        statusBar_.clearDirty();
        mainMenu_.clearDirty();
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
        stopRawCapture();
        stopJammer();
        EventSystem::instance().unsubscribe(this);
        ESP_LOGI(TAG_RF_APP, "destroyed");
    }

private:
    static constexpr size_t LINE_LEN = 32U;
    static constexpr size_t LINE_BUF = 256U;

    StatusBar statusBar_;
    MenuListView mainMenu_;
    RFState state_;
    bool needsRedraw_;
    bool jammerActive_;

    /// Local copy of captured pulse timings (copied from the ISR buffer).
    int32_t  capturedTimings_[RAW_BUF_CAPACITY];
    uint16_t capturedCount_;

    /// Status message shown on save / replay / load screens.
    char statusLine_[LINE_LEN];

    // ── State transitions ────────────────────────────────────────────────

    void transitionTo(RFState next)
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

    /// Live capture screen: waveform + pulse counter.
    void drawCaptureView()
    {
        if (capturedCount_ > 0U)
        {
            drawWaveform();
        }
        else
        {
            DisplayManager::instance().drawText(2, 30, "Listening...");
            DisplayManager::instance().drawText(2, 56, "Press to stop");
        }
    }

    /**
     * @brief Draw a mini-oscilloscope representation of the captured signal.
     *
     * The total duration of all captured pulses is scaled to fit the 125-pixel
     * display width.  Marks are drawn at the top rail and spaces at the bottom
     * rail, with vertical transition lines connecting them.
     */
    void drawWaveform()
    {
        if (capturedCount_ == 0U)
        {
            DisplayManager::instance().drawText(2, 30, "No signal data");
            return;
        }

        // Total absolute duration for horizontal scaling.
        int64_t totalDuration = 0;
        for (uint16_t i = 0U; i < capturedCount_; ++i)
        {
            const int32_t t = capturedTimings_[i];
            totalDuration += (t > 0) ? t : -t;
        }

        if (totalDuration == 0)
        {
            return;
        }

        int16_t x = WAVE_X0;
        bool prevHigh = (capturedTimings_[0] > 0);

        for (uint16_t i = 0U; i < capturedCount_ && x < WAVE_X1; ++i)
        {
            const int32_t timing = capturedTimings_[i];
            const bool isHigh = (timing > 0);
            const uint32_t absDur = static_cast<uint32_t>(
                timing > 0 ? timing : -timing);

            int16_t pxWidth = static_cast<int16_t>(
                (static_cast<int64_t>(absDur) * WAVE_WIDTH) / totalDuration);
            if (pxWidth < 1)
            {
                pxWidth = 1;
            }

            const int16_t y = isHigh ? WAVE_Y_HIGH : WAVE_Y_LOW;

            // Vertical transition line between HIGH ↔ LOW.
            if (i > 0U && isHigh != prevHigh)
            {
                DisplayManager::instance().drawLine(x, WAVE_Y_HIGH, x, WAVE_Y_LOW);
            }

            // Horizontal pulse line.
            int16_t xEnd = x + pxWidth;
            if (xEnd > WAVE_X1)
            {
                xEnd = WAVE_X1;
            }
            DisplayManager::instance().drawLine(x, y, xEnd, y);

            x = xEnd;
            prevHigh = isHigh;
        }

        char info[LINE_LEN];
        const int64_t totalMs = totalDuration / 1000;
        std::snprintf(info, sizeof(info), "%u pulses  %ldms",
                      capturedCount_, static_cast<long>(totalMs));
        DisplayManager::instance().drawText(2, 56, info);
    }

    void drawJammerView()
    {
        DisplayManager::instance().drawText(2, 24, "!! WARNING !!");
        DisplayManager::instance().drawText(2, 36, "TX on 433.92 MHz");
        DisplayManager::instance().drawText(2, 48, "Press to stop");
    }

    void drawStatusView()
    {
        DisplayManager::instance().drawText(2, 32, statusLine_);
        DisplayManager::instance().drawText(2, 48, "Press to continue");
    }

    // ── RAW capture control (ISR-based) ──────────────────────────────────

    void startRawCapture()
    {
        s_rawCount   = 0U;
        s_lastEdgeUs = 0;
        capturedCount_ = 0U;

        gpio_config_t io_conf = {};
        io_conf.pin_bit_mask  = (1ULL << RF_RX_GPIO);
        io_conf.mode          = GPIO_MODE_INPUT;
        io_conf.pull_up_en    = GPIO_PULLUP_DISABLE;
        io_conf.pull_down_en  = GPIO_PULLDOWN_DISABLE;
        io_conf.intr_type     = GPIO_INTR_ANYEDGE;
        gpio_config(&io_conf);

        gpio_isr_handler_add(RF_RX_GPIO, rfRxEdgeISR, nullptr);
        s_isrActive = true;

        ESP_LOGI(TAG_RF_APP, "RAW capture started on GPIO%u",
                 static_cast<unsigned>(PIN_RF_RX));
    }

    void stopRawCapture()
    {
        if (s_isrActive)
        {
            gpio_isr_handler_remove(RF_RX_GPIO);
            gpio_set_intr_type(RF_RX_GPIO, GPIO_INTR_DISABLE);
            s_isrActive = false;

            copyIsrBuffer(s_rawCount);

            ESP_LOGI(TAG_RF_APP, "RAW capture stopped – %u pulses",
                     capturedCount_);
        }
    }

    /// Copy the volatile ISR buffer into local (non-volatile) storage.
    void copyIsrBuffer(uint16_t count)
    {
        if (count > RAW_BUF_CAPACITY)
        {
            count = static_cast<uint16_t>(RAW_BUF_CAPACITY);
        }
        for (uint16_t i = 0U; i < count; ++i)
        {
            capturedTimings_[i] = s_rawTimings[i];
        }
        capturedCount_ = count;
    }

    // ── Jammer control (LEDC PWM) ────────────────────────────────────────

    void startJammer()
    {
        ledcSetup(LEDC_JAMMER_CHANNEL, JAMMER_FREQ_HZ, 1U);
        ledcAttachPin(PIN_RF_TX, LEDC_JAMMER_CHANNEL);
        ledcWrite(LEDC_JAMMER_CHANNEL, 1U); // 50 % duty at 1-bit resolution

        jammerActive_ = true;
        ESP_LOGI(TAG_RF_APP, "Jammer started on GPIO%u at %luHz",
                 static_cast<unsigned>(PIN_RF_TX),
                 static_cast<unsigned long>(JAMMER_FREQ_HZ));
    }

    void stopJammer()
    {
        if (jammerActive_)
        {
            ledcWrite(LEDC_JAMMER_CHANNEL, 0U);
            ledcDetachPin(PIN_RF_TX);
            jammerActive_ = false;
            ESP_LOGI(TAG_RF_APP, "Jammer stopped");
        }
    }

    // ── Save signal to Flipper-compatible .sub file ──────────────────────

    bool saveSignal()
    {
        if (capturedCount_ == 0U)
        {
            return false;
        }

        auto &vfs = hackos::storage::VirtualFS::instance();
        fs::File f = vfs.open(CAPTURE_FILE_PATH, "w");
        if (!f)
        {
            ESP_LOGE(TAG_RF_APP, "Cannot open %s for writing", CAPTURE_FILE_PATH);
            return false;
        }

        f.println("Filetype: Flipper SubGhz RAW File");
        f.println("Version: 1");
        f.printf("Frequency: %lu\n", static_cast<unsigned long>(RF_FREQUENCY_HZ));
        f.println("Preset: FuriHalSubGhzPresetOok650Async");
        f.println("Protocol: RAW");

        for (uint16_t i = 0U; i < capturedCount_;)
        {
            f.print("RAW_Data:");
            for (size_t j = 0U; j < SUB_VALUES_PER_LINE && i < capturedCount_; ++j, ++i)
            {
                f.printf(" %ld", static_cast<long>(capturedTimings_[i]));
            }
            f.println();
        }

        f.close();
        ESP_LOGI(TAG_RF_APP, "Saved %u pulses to %s", capturedCount_, CAPTURE_FILE_PATH);
        return true;
    }

    // ── Load signal from .sub file ───────────────────────────────────────

    bool loadSignal()
    {
        auto &vfs = hackos::storage::VirtualFS::instance();
        fs::File f = vfs.open(CAPTURE_FILE_PATH, "r");
        if (!f)
        {
            ESP_LOGE(TAG_RF_APP, "Cannot open %s for reading", CAPTURE_FILE_PATH);
            return false;
        }

        capturedCount_ = 0U;
        char line[LINE_BUF];

        while (f.available() && capturedCount_ < RAW_BUF_CAPACITY)
        {
            const size_t len = f.readBytesUntil('\n', line, sizeof(line) - 1U);
            line[len] = '\0';

            if (std::strncmp(line, "RAW_Data:", 9) != 0)
            {
                continue;
            }

            char *p = line + 9;
            while (*p != '\0' && capturedCount_ < RAW_BUF_CAPACITY)
            {
                while (*p == ' ' || *p == '\t')
                {
                    ++p;
                }
                if (*p == '\0' || *p == '\r' || *p == '\n')
                {
                    break;
                }

                char *end = nullptr;
                const long val = std::strtol(p, &end, 10);
                if (end == p)
                {
                    break;
                }

                capturedTimings_[capturedCount_] = static_cast<int32_t>(val);
                ++capturedCount_;
                p = end;
            }
        }

        f.close();
        ESP_LOGI(TAG_RF_APP, "Loaded %u pulses from %s", capturedCount_, CAPTURE_FILE_PATH);
        return capturedCount_ > 0U;
    }

    // ── Replay captured signal (exact timing reproduction) ───────────────

    void replaySignal()
    {
        if (capturedCount_ == 0U)
        {
            return;
        }

        gpio_config_t io_conf = {};
        io_conf.pin_bit_mask  = (1ULL << RF_TX_GPIO);
        io_conf.mode          = GPIO_MODE_OUTPUT;
        io_conf.pull_up_en    = GPIO_PULLUP_DISABLE;
        io_conf.pull_down_en  = GPIO_PULLDOWN_DISABLE;
        io_conf.intr_type     = GPIO_INTR_DISABLE;
        gpio_config(&io_conf);

        ESP_LOGI(TAG_RF_APP, "Replaying %u pulses", capturedCount_);

        for (uint16_t i = 0U; i < capturedCount_; ++i)
        {
            const int32_t timing = capturedTimings_[i];
            const bool level = (timing > 0);
            const uint32_t duration = static_cast<uint32_t>(
                timing > 0 ? timing : -timing);

            gpio_set_level(RF_TX_GPIO, level ? 1 : 0);
            delayMicroseconds(duration);
        }

        gpio_set_level(RF_TX_GPIO, 0);
        ESP_LOGI(TAG_RF_APP, "Replay complete");
    }

    // ── Input routing ────────────────────────────────────────────────────

    void handleInput(InputManager::InputEvent input)
    {
        switch (state_)
        {
        case RFState::MAIN_MENU:
            handleMainMenu(input);
            break;

        case RFState::RAW_CAPTURE:
            if (input == InputManager::InputEvent::BUTTON_PRESS ||
                input == InputManager::InputEvent::LEFT)
            {
                stopRawCapture();
                transitionTo(capturedCount_ > 0U
                                 ? RFState::SIGNAL_VIEW
                                 : RFState::MAIN_MENU);
                if (state_ == RFState::MAIN_MENU)
                {
                    mainMenu_.setItems(RF_MENU_LABELS, RF_MENU_COUNT);
                }
            }
            break;

        case RFState::SIGNAL_VIEW:
        case RFState::JAMMING:
            if (input == InputManager::InputEvent::BUTTON_PRESS ||
                input == InputManager::InputEvent::LEFT)
            {
                if (state_ == RFState::JAMMING)
                {
                    stopJammer();
                }
                transitionTo(RFState::MAIN_MENU);
                mainMenu_.setItems(RF_MENU_LABELS, RF_MENU_COUNT);
            }
            break;

        case RFState::SAVING:
        case RFState::REPLAYING:
            if (input == InputManager::InputEvent::BUTTON_PRESS ||
                input == InputManager::InputEvent::LEFT)
            {
                transitionTo(RFState::MAIN_MENU);
                mainMenu_.setItems(RF_MENU_LABELS, RF_MENU_COUNT);
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

            switch (sel)
            {
            case 0U: // RAW Capture
                startRawCapture();
                transitionTo(RFState::RAW_CAPTURE);
                break;

            case 1U: // Jammer
                startJammer();
                transitionTo(RFState::JAMMING);
                break;

            case 2U: // Save Signal
                if (capturedCount_ > 0U)
                {
                    if (saveSignal())
                    {
                        std::snprintf(statusLine_, sizeof(statusLine_),
                                      "Saved %u pulses", capturedCount_);
                    }
                    else
                    {
                        std::snprintf(statusLine_, sizeof(statusLine_),
                                      "Save failed!");
                    }
                }
                else
                {
                    std::snprintf(statusLine_, sizeof(statusLine_),
                                  "No signal to save");
                }
                transitionTo(RFState::SAVING);
                break;

            case 3U: // Replay Signal
                if (capturedCount_ > 0U)
                {
                    replaySignal();
                    std::snprintf(statusLine_, sizeof(statusLine_),
                                  "Replay complete!");
                }
                else
                {
                    std::snprintf(statusLine_, sizeof(statusLine_),
                                  "No signal to replay");
                }
                transitionTo(RFState::REPLAYING);
                break;

            case 4U: // Load Signal
                if (loadSignal())
                {
                    transitionTo(RFState::SIGNAL_VIEW);
                }
                else
                {
                    std::snprintf(statusLine_, sizeof(statusLine_),
                                  "Load failed!");
                    transitionTo(RFState::SAVING);
                }
                break;

            case 5U: // Back
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
};

} // namespace

// ── Factory function ─────────────────────────────────────────────────────────

AppBase *createRFToolsApp()
{
    return new (std::nothrow) RFToolsApp();
}
