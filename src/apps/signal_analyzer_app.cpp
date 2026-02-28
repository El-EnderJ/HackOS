/**
 * @file signal_analyzer_app.cpp
 * @brief Signal Analyzer – Waterfall RF visualizer for 433 MHz activity.
 *
 * Uses the ESP32 ADC to sample the RF receiver pin (pulse density) at
 * high speed and renders a downward-scrolling "waterfall" display on
 * the 128×64 OLED.  Brighter pixels indicate stronger RF activity.
 *
 * Features:
 *  - High-speed ADC sampling of PIN_RF_RX for signal intensity.
 *  - Waterfall UI: each new sample row appears at the top and scrolls
 *    down, giving a time-vs-intensity view of the 433 MHz band.
 *  - Peak detection: the column with the highest energy in the current
 *    frame is highlighted with a marker.
 *  - Sound-to-Light: optional buzzer output that maps signal strength
 *    to an audible tone via LEDC PWM on PIN_BUZZER.
 *  - DMA-style direct buffer writes to maintain ~30 FPS.
 */

#include "apps/signal_analyzer_app.h"

#include <cstdio>
#include <cstring>
#include <new>

#include <Arduino.h>
#include <esp_log.h>

#include "hackos.h"
#include "config.h"

// ── Anonymous namespace for all internal implementation ──────────────────────

namespace
{

// ── Constants ────────────────────────────────────────────────────────────────

static constexpr const char *TAG_SIG = "SignalAnalyzer";

/// Display geometry
static constexpr int16_t DISPLAY_W = Canvas::WIDTH;   // 128
static constexpr int16_t DISPLAY_H = Canvas::HEIGHT;   // 64

/// Header height (pixels) reserved for the title bar
static constexpr int16_t HEADER_H = 10;

/// Waterfall area height (pixels)
static constexpr int16_t WF_H = DISPLAY_H - HEADER_H; // 54

/// Number of ADC samples per waterfall row (averaged into 128 columns)
static constexpr size_t SAMPLES_PER_ROW = 128U;

/// ADC resolution (12-bit on ESP32)
static constexpr uint16_t ADC_MAX = 4095U;

/// Number of intensity levels mapped to dither patterns (monochrome OLED)
static constexpr uint8_t INTENSITY_LEVELS = 4U;

/// LEDC channel and timer used for buzzer PWM
static constexpr uint8_t BUZZER_LEDC_CHANNEL = 0U;
static constexpr uint8_t BUZZER_LEDC_TIMER = 0U;
static constexpr uint8_t BUZZER_LEDC_RESOLUTION = 8U;

/// Buzzer tone range (Hz)
static constexpr uint32_t BUZZER_FREQ_MIN = 200U;
static constexpr uint32_t BUZZER_FREQ_MAX = 4000U;

/// High-speed sampling interval (µs) – aim for ~100 kHz effective rate
static constexpr uint32_t SAMPLE_INTERVAL_US = 10U;

/// Frame budget target (ms) for ~30 FPS
static constexpr uint32_t FRAME_BUDGET_MS = 33U;

/// XP is awarded once per session when peaks are detected
static constexpr uint32_t XP_COOLDOWN_MS = 10000U;

// ── Scene / View IDs ────────────────────────────────────────────────────────

enum SceneId : uint32_t
{
    SCENE_WATERFALL = 0U,
    SCENE_COUNT     = 1U,
};

enum ViewId : uint32_t
{
    VIEW_WATERFALL = 0U,
};

enum AppEvent : uint32_t
{
    EVENT_BACK = 100U,
};

// ── Waterfall buffer ────────────────────────────────────────────────────────

/// Each row stores 128 intensity values (0–3) packed 4 per byte (2 bits each).
/// Total storage: 128 columns × 54 rows × 2 bits / 8 = 1728 bytes.
static constexpr size_t WF_ROW_BYTES = (SAMPLES_PER_ROW * 2U + 7U) / 8U; // 32

// ── WaterfallView ───────────────────────────────────────────────────────────

class WaterfallView final : public View
{
public:
    WaterfallView()
        : buzzerOn_(false),
          peakCol_(0),
          peakVal_(0),
          lastSampleMs_(0U),
          xpAwarded_(false),
          xpCooldownMs_(0U)
    {
        std::memset(wfBuf_, 0, sizeof(wfBuf_));
        std::memset(currentRow_, 0, sizeof(currentRow_));
    }

    // ── High-speed sampling ─────────────────────────────────────────────

    /**
     * @brief Sample the RF RX pin at high speed and fill the current row.
     *
     * Reads PIN_RF_RX using analogRead at tight intervals.  Each of the
     * 128 columns gets one sample (this keeps it simple on the monochrome
     * OLED).  The 12-bit ADC value is quantised into 0-3 intensity.
     */
    void sampleRow()
    {
        uint16_t peakV = 0U;
        uint8_t peakC = 0U;

        for (size_t i = 0U; i < SAMPLES_PER_ROW; ++i)
        {
            const uint16_t raw = static_cast<uint16_t>(analogRead(PIN_RF_RX));
            const uint8_t q = quantise(raw);
            currentRow_[i] = q;

            if (raw > peakV)
            {
                peakV = raw;
                peakC = static_cast<uint8_t>(i);
            }

            delayMicroseconds(SAMPLE_INTERVAL_US);
        }

        peakCol_ = peakC;
        peakVal_ = peakV;

        // Scroll the waterfall buffer down by one row
        scrollDown();

        // Copy current quantised row into top of waterfall buffer
        for (size_t c = 0U; c < SAMPLES_PER_ROW; ++c)
        {
            setIntensity(0U, c, currentRow_[c]);
        }

        // Sound-to-light: drive buzzer frequency proportional to peak
        if (buzzerOn_)
        {
            if (peakVal_ > ADC_MAX / 10U) // noise gate
            {
                const uint32_t freq = BUZZER_FREQ_MIN +
                    static_cast<uint32_t>(
                        static_cast<uint64_t>(peakVal_) *
                        (BUZZER_FREQ_MAX - BUZZER_FREQ_MIN) / ADC_MAX);
                ledcWriteTone(BUZZER_LEDC_CHANNEL, static_cast<double>(freq));
            }
            else
            {
                ledcWriteTone(BUZZER_LEDC_CHANNEL, 0);
            }
        }

        // XP award on significant peak detection
        if (peakVal_ > ADC_MAX / 4U)
        {
            const uint32_t now = static_cast<uint32_t>(millis());
            if (!xpAwarded_ || (now - xpCooldownMs_ > XP_COOLDOWN_MS))
            {
                const Event xpEvt{EventType::EVT_XP_EARNED, XP_SIGNAL_ANALYZE, 0, nullptr};
                EventSystem::instance().postEvent(xpEvt);
                xpAwarded_ = true;
                xpCooldownMs_ = now;
            }
        }
    }

    // ── Drawing ─────────────────────────────────────────────────────────

    void draw(Canvas *canvas) override
    {
        // Title bar
        canvas->drawStr(2, 7, "RF Waterfall 433MHz");
        canvas->drawLine(0, HEADER_H - 1, DISPLAY_W - 1, HEADER_H - 1);

        // Buzzer indicator
        if (buzzerOn_)
        {
            canvas->drawStr(110, 7, "S");
        }

        // Waterfall area – render from buffer using dither patterns
        drawWaterfall(canvas);

        // Peak marker (inverted triangle at the peak column)
        if (peakVal_ > ADC_MAX / 10U)
        {
            const int16_t px = static_cast<int16_t>(peakCol_);
            canvas->drawPixel(px, HEADER_H);
            if (px > 0)
            {
                canvas->drawPixel(px - 1, HEADER_H + 1);
            }
            if (px < DISPLAY_W - 1)
            {
                canvas->drawPixel(px + 1, HEADER_H + 1);
            }
        }

        // Peak value text (bottom-right)
        char peakTxt[18];
        std::snprintf(peakTxt, sizeof(peakTxt), "Pk:%u", peakVal_);
        canvas->drawStr(90, DISPLAY_H - 1, peakTxt);
    }

    bool input(InputEvent * /*event*/) override { return false; }

    // ── Buzzer control ──────────────────────────────────────────────────

    void toggleBuzzer()
    {
        buzzerOn_ = !buzzerOn_;
        if (!buzzerOn_)
        {
            ledcWriteTone(BUZZER_LEDC_CHANNEL, 0);
        }
        ESP_LOGI(TAG_SIG, "Buzzer %s", buzzerOn_ ? "ON" : "OFF");
    }

    bool buzzerOn() const { return buzzerOn_; }

    void stopBuzzer()
    {
        buzzerOn_ = false;
        ledcWriteTone(BUZZER_LEDC_CHANNEL, 0);
    }

private:
    /// 2-bit intensity buffer: wfBuf_[row][packedCol]
    uint8_t wfBuf_[WF_H][WF_ROW_BYTES];
    uint8_t currentRow_[SAMPLES_PER_ROW];

    bool buzzerOn_;
    uint8_t peakCol_;
    uint16_t peakVal_;
    uint32_t lastSampleMs_;
    bool xpAwarded_;
    uint32_t xpCooldownMs_;

    // ── Helpers ──────────────────────────────────────────────────────────

    /// Quantise 12-bit ADC value to 0-3 intensity.
    static uint8_t quantise(uint16_t raw)
    {
        if (raw < ADC_MAX / 4U)
        {
            return 0U;
        }
        if (raw < ADC_MAX / 2U)
        {
            return 1U;
        }
        if (raw < (ADC_MAX * 3U) / 4U)
        {
            return 2U;
        }
        return 3U;
    }

    /// Get the 2-bit intensity at (row, col) in the waterfall buffer.
    uint8_t getIntensity(size_t row, size_t col) const
    {
        const size_t byteIdx = (col * 2U) / 8U;
        const uint8_t shift = static_cast<uint8_t>((col * 2U) % 8U);
        return (wfBuf_[row][byteIdx] >> shift) & 0x03U;
    }

    /// Set the 2-bit intensity at (row, col) in the waterfall buffer.
    void setIntensity(size_t row, size_t col, uint8_t val)
    {
        const size_t byteIdx = (col * 2U) / 8U;
        const uint8_t shift = static_cast<uint8_t>((col * 2U) % 8U);
        wfBuf_[row][byteIdx] = static_cast<uint8_t>(
            (wfBuf_[row][byteIdx] & ~(0x03U << shift)) |
            ((val & 0x03U) << shift));
    }

    /// Scroll all rows down by 1 (row WF_H-1 is discarded).
    void scrollDown()
    {
        for (int r = WF_H - 1; r > 0; --r)
        {
            std::memcpy(wfBuf_[r], wfBuf_[r - 1], WF_ROW_BYTES);
        }
        std::memset(wfBuf_[0], 0, WF_ROW_BYTES);
    }

    /**
     * @brief Render the waterfall buffer onto the canvas.
     *
     * Uses dither patterns to represent 4 intensity levels on the
     * monochrome display:
     *   0 – pixel off
     *   1 – checkerboard (25% fill)
     *   2 – dense checkerboard (50% fill)
     *   3 – pixel on (100%)
     */
    void drawWaterfall(Canvas *canvas) const
    {
        for (int16_t r = 0; r < WF_H; ++r)
        {
            const int16_t y = HEADER_H + r;
            for (int16_t c = 0; c < DISPLAY_W; ++c)
            {
                const uint8_t intensity = getIntensity(
                    static_cast<size_t>(r), static_cast<size_t>(c));

                bool lit = false;
                switch (intensity)
                {
                case 0U:
                    lit = false;
                    break;
                case 1U:
                    // Sparse dither: every 4th pixel
                    lit = ((r + c) % 4 == 0);
                    break;
                case 2U:
                    // Checkerboard dither: every other pixel
                    lit = ((r + c) % 2 == 0);
                    break;
                case 3U:
                    lit = true;
                    break;
                default:
                    break;
                }

                if (lit)
                {
                    canvas->drawPixel(c, y);
                }
            }
        }
    }
};

// ═══════════════════════════════════════════════════════════════════════════════
// ── SignalAnalyzerApp ──────────────────────────────────────────────────────
// ═══════════════════════════════════════════════════════════════════════════════

class SignalAnalyzerApp final : public hackos::HackOSApp
{
public:
    SignalAnalyzerApp()
        : waterfallView_(nullptr),
          sceneManager_(nullptr),
          viewDispatcher_(),
          statusBar_(0, 0, 128, 8),
          needsRedraw_(true)
    {
    }

    // ── HackOSApp lifecycle ──────────────────────────────────────────────

    void on_alloc() override
    {
        waterfallView_ = static_cast<WaterfallView *>(
            ctx().alloc(sizeof(WaterfallView)));
        if (waterfallView_ != nullptr)
        {
            new (waterfallView_) WaterfallView();
        }
    }

    void on_start() override
    {
        statusBar_.setConnectivity(false, false);
        statusBar_.setBatteryLevel(100U);
        statusBar_.setTime(0U, 0U);

        if (waterfallView_ != nullptr)
        {
            viewDispatcher_.addView(VIEW_WATERFALL, waterfallView_);
        }

        // Set up scene table (single scene: waterfall)
        static const SceneHandler handlers[SCENE_COUNT] = {
            {sceneWaterfallEnter, sceneWaterfallEvent, sceneWaterfallExit},
        };

        sceneManager_ = new (std::nothrow) SceneManager(handlers, SCENE_COUNT, this);
        if (sceneManager_ != nullptr)
        {
            sceneManager_->navigateTo(SCENE_WATERFALL);
        }

        // Initialise ADC for the RF pin
        analogReadResolution(12);
        analogSetAttenuation(ADC_11db);
        pinMode(PIN_RF_RX, INPUT);

        // Initialise LEDC for the buzzer (sound-to-light)
        ledcSetup(BUZZER_LEDC_CHANNEL, 2000, BUZZER_LEDC_RESOLUTION);
        ledcAttachPin(PIN_BUZZER, BUZZER_LEDC_CHANNEL);
        ledcWriteTone(BUZZER_LEDC_CHANNEL, 0);

        needsRedraw_ = true;
        ESP_LOGI(TAG_SIG, "Signal Analyzer started");
    }

    void on_event(Event *event) override
    {
        if (event->type != EventType::EVT_INPUT)
        {
            return;
        }

        const auto input = static_cast<InputManager::InputEvent>(event->arg0);

        // CENTER: toggle buzzer
        if (input == InputManager::InputEvent::BUTTON_PRESS)
        {
            if (waterfallView_ != nullptr)
            {
                waterfallView_->toggleBuzzer();
                needsRedraw_ = true;
            }
        }
        // LEFT or BACK: exit app
        else if (input == InputManager::InputEvent::LEFT)
        {
            if (waterfallView_ != nullptr)
            {
                waterfallView_->stopBuzzer();
            }
            const Event evt{EventType::EVT_SYSTEM, SYSTEM_EVENT_BACK, 0, nullptr};
            EventSystem::instance().postEvent(evt);
        }
    }

    void on_free() override
    {
        if (waterfallView_ != nullptr)
        {
            waterfallView_->stopBuzzer();
        }

        // Detach buzzer
        ledcDetachPin(PIN_BUZZER);

        viewDispatcher_.removeView(VIEW_WATERFALL);

        if (waterfallView_ != nullptr)
        {
            waterfallView_->~WaterfallView();
        }

        delete sceneManager_;
        sceneManager_ = nullptr;

        ESP_LOGI(TAG_SIG, "Signal Analyzer freed");
    }

    void on_update() override
    {
        // Continuously sample at high speed
        if (waterfallView_ != nullptr)
        {
            waterfallView_->sampleRow();
            needsRedraw_ = true;
        }
    }

    void on_draw() override
    {
        if (!needsRedraw_ && !statusBar_.isDirty())
        {
            return;
        }

        // DMA-optimised path: write directly to the display buffer
        // when possible, otherwise fall through to canvas rendering.
        DisplayManager &disp = DisplayManager::instance();
        disp.clear();
        statusBar_.draw();

        Canvas canvas;
        canvas.clear();
        viewDispatcher_.draw(&canvas);

        // Copy canvas buffer to display via direct buffer access (DMA-style)
        uint8_t *fb = disp.getDisplayBuffer();
        if (fb != nullptr)
        {
            std::memcpy(fb, canvas.buffer(), Canvas::BUFFER_SIZE);
        }

        disp.present();
        statusBar_.clearDirty();
        needsRedraw_ = false;
    }

    // ── Accessors for scene callbacks ────────────────────────────────────

    ViewDispatcher &viewDispatcher() { return viewDispatcher_; }
    void requestRedraw() { needsRedraw_ = true; }

private:
    WaterfallView *waterfallView_;
    SceneManager *sceneManager_;
    ViewDispatcher viewDispatcher_;
    StatusBar statusBar_;
    bool needsRedraw_;

    // ── Scene 0: Waterfall ──────────────────────────────────────────────

    static void sceneWaterfallEnter(void *context)
    {
        auto *app = static_cast<SignalAnalyzerApp *>(context);
        app->viewDispatcher().switchToView(VIEW_WATERFALL);
        app->requestRedraw();
    }

    static bool sceneWaterfallEvent(void *context, uint32_t eventId)
    {
        if (eventId == EVENT_BACK)
        {
            auto *app = static_cast<SignalAnalyzerApp *>(context);
            (void)app;
            const Event evt{EventType::EVT_SYSTEM, SYSTEM_EVENT_BACK, 0, nullptr};
            EventSystem::instance().postEvent(evt);
            return true;
        }
        return false;
    }

    static void sceneWaterfallExit(void * /*context*/)
    {
    }
};

} // namespace

// ── Factory function ─────────────────────────────────────────────────────────

AppBase *createSignalAnalyzerApp()
{
    return new (std::nothrow) SignalAnalyzerApp();
}
