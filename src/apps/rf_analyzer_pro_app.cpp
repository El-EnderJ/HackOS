/**
 * @file rf_analyzer_pro_app.cpp
 * @brief RFAnalyzerPro – Signal Waterfall Visualizer for advanced 433 MHz
 *        RF analysis.
 *
 * Visualises the 433 MHz RF data pin as a real-time scrolling waterfall
 * (spectrogram).  Each horizontal line of pixels represents one snapshot in
 * time: black (lit) pixels denote signal-HIGH, white (off) pixels denote
 * signal-LOW.  Rows scroll downward so the newest sample is always at the
 * top.
 *
 * Features:
 *  - Fast Sampling: optimised digital bit-banging of PIN_RF_RX at the
 *    maximum speed the ESP32 GPIO port register allows.
 *  - Waterfall Display: 128×54 pixel scrolling bitmap on the OLED.
 *  - Time Scaling (Zoom): UP / DOWN changes the inter-sample delay so the
 *    user can inspect short pulses (zoom in) or long patterns (zoom out).
 *  - Pause / Resume: CENTER button freezes the display for closer study.
 */

#include "apps/rf_analyzer_pro_app.h"

#include <cstdio>
#include <cstring>
#include <new>

#include <Arduino.h>
#include <esp_log.h>
#include <driver/gpio.h>

#include "hackos.h"
#include "config.h"

// ── Anonymous namespace for all internal implementation ──────────────────────

namespace
{

// ── Constants ────────────────────────────────────────────────────────────────

static constexpr const char *TAG_RFA = "RFAnalyzerPro";

/// Display geometry
static constexpr int16_t DISPLAY_W = Canvas::WIDTH;   // 128
static constexpr int16_t DISPLAY_H = Canvas::HEIGHT;   // 64

/// Header height (pixels) reserved for the status / title bar
static constexpr int16_t HEADER_H = 10;

/// Waterfall area height (pixels)
static constexpr int16_t WF_H = DISPLAY_H - HEADER_H; // 54

/// Number of digital samples per waterfall row (one per column)
static constexpr size_t SAMPLES_PER_ROW = 128U;

/// Packed row width: 128 bits / 8 = 16 bytes per row
static constexpr size_t WF_ROW_BYTES = SAMPLES_PER_ROW / 8U; // 16

/// XP cooldown between awards (ms)
static constexpr uint32_t XP_COOLDOWN_MS = 10000U;

// ── Zoom / time-scale levels ────────────────────────────────────────────────

/// Number of available zoom presets
static constexpr size_t ZOOM_LEVELS = 6U;

/// Inter-sample delay in microseconds for each zoom level.
/// Lower value → faster sampling → zoomed in on short pulses.
static constexpr uint16_t ZOOM_US[ZOOM_LEVELS] = {
    1U,    // Level 0: ~1 µs   (fastest – short pulses)
    5U,    // Level 1: ~5 µs
    10U,   // Level 2: ~10 µs  (default)
    50U,   // Level 3: ~50 µs
    100U,  // Level 4: ~100 µs
    500U,  // Level 5: ~500 µs (widest – long patterns)
};

/// Human-readable labels shown in the header
static constexpr const char *ZOOM_LABELS[ZOOM_LEVELS] = {
    "1us", "5us", "10us", "50us", "100us", "500us",
};

/// Default zoom index
static constexpr size_t DEFAULT_ZOOM = 2U;

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

// ── RFWaterfallView ─────────────────────────────────────────────────────────

/**
 * @class RFWaterfallView
 * @brief View that performs high-speed digital sampling and renders the
 *        waterfall bitmap.
 */
class RFWaterfallView final : public View
{
public:
    RFWaterfallView()
        : zoomIdx_(DEFAULT_ZOOM),
          paused_(false),
          signalDetected_(false),
          xpAwarded_(false),
          xpCooldownMs_(0U)
    {
        std::memset(wfBuf_, 0, sizeof(wfBuf_));
    }

    // ── High-speed bit-bang sampling ────────────────────────────────────

    /**
     * @brief Sample the RF RX pin digitally at high speed.
     *
     * Reads PIN_RF_RX using direct GPIO register access for maximum
     * throughput.  128 samples are captured at the current zoom interval
     * and stored as a packed bit row, then the waterfall scrolls down.
     */
    void sampleRow()
    {
        if (paused_)
        {
            return;
        }

        const uint16_t delayUs = ZOOM_US[zoomIdx_];
        const gpio_num_t pin = static_cast<gpio_num_t>(PIN_RF_RX);
        uint8_t row[WF_ROW_BYTES];
        std::memset(row, 0, sizeof(row));

        bool anyHigh = false;

        for (size_t i = 0U; i < SAMPLES_PER_ROW; ++i)
        {
            // Direct register read for speed
            const int val = gpio_get_level(pin);
            if (val != 0)
            {
                row[i / 8U] |= static_cast<uint8_t>(1U << (i % 8U));
                anyHigh = true;
            }

            if (delayUs > 0U)
            {
                delayMicroseconds(delayUs);
            }
        }

        signalDetected_ = anyHigh;

        // Scroll waterfall buffer down by one row
        scrollDown();

        // Place new row at the top
        std::memcpy(wfBuf_[0], row, WF_ROW_BYTES);

        // XP award when signal activity is detected
        if (anyHigh)
        {
            const uint32_t now = static_cast<uint32_t>(millis());
            if (!xpAwarded_ || (now - xpCooldownMs_ > XP_COOLDOWN_MS))
            {
                const Event xpEvt{EventType::EVT_XP_EARNED,
                                  XP_SIGNAL_ANALYZE, 0, nullptr};
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
        canvas->drawStr(2, 7, "RF Pro 433MHz");

        // Zoom indicator
        char zoomTxt[16];
        std::snprintf(zoomTxt, sizeof(zoomTxt), "Z:%s",
                      ZOOM_LABELS[zoomIdx_]);
        canvas->drawStr(80, 7, zoomTxt);

        // Pause indicator
        if (paused_)
        {
            canvas->drawStr(118, 7, "P");
        }
        // Signal indicator
        else if (signalDetected_)
        {
            canvas->drawStr(122, 7, "*");
        }

        // Header separator line
        canvas->drawLine(0, HEADER_H - 1, DISPLAY_W - 1, HEADER_H - 1);

        // Waterfall area – render packed bit rows
        drawWaterfall(canvas);
    }

    bool input(InputEvent * /*event*/) override { return false; }

    // ── Controls ────────────────────────────────────────────────────────

    void zoomIn()
    {
        if (zoomIdx_ > 0U)
        {
            --zoomIdx_;
            ESP_LOGI(TAG_RFA, "Zoom in: %s", ZOOM_LABELS[zoomIdx_]);
        }
    }

    void zoomOut()
    {
        if (zoomIdx_ < ZOOM_LEVELS - 1U)
        {
            ++zoomIdx_;
            ESP_LOGI(TAG_RFA, "Zoom out: %s", ZOOM_LABELS[zoomIdx_]);
        }
    }

    void togglePause()
    {
        paused_ = !paused_;
        ESP_LOGI(TAG_RFA, "Capture %s", paused_ ? "PAUSED" : "RUNNING");
    }

    bool isPaused() const { return paused_; }
    size_t zoomIndex() const { return zoomIdx_; }

private:
    /// Packed 1-bit waterfall buffer: wfBuf_[row][packedByte]
    uint8_t wfBuf_[WF_H][WF_ROW_BYTES];

    size_t zoomIdx_;
    bool paused_;
    bool signalDetected_;
    bool xpAwarded_;
    uint32_t xpCooldownMs_;

    // ── Helpers ──────────────────────────────────────────────────────────

    /// Scroll all rows down by 1 (bottom row is discarded).
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
     * Each bit in the buffer corresponds to a pixel on the display.
     * Bit = 1 → pixel ON (black on typical OLED) → signal HIGH.
     * Bit = 0 → pixel OFF (dark/white) → signal LOW.
     */
    void drawWaterfall(Canvas *canvas) const
    {
        for (int16_t r = 0; r < WF_H; ++r)
        {
            const int16_t y = HEADER_H + r;
            for (int16_t c = 0; c < DISPLAY_W; ++c)
            {
                const size_t byteIdx = static_cast<size_t>(c) / 8U;
                const uint8_t bitIdx =
                    static_cast<uint8_t>(static_cast<size_t>(c) % 8U);
                if ((wfBuf_[r][byteIdx] >> bitIdx) & 0x01U)
                {
                    canvas->drawPixel(c, y);
                }
            }
        }
    }
};

// ═══════════════════════════════════════════════════════════════════════════════
// ── RFAnalyzerProApp ──────────────────────────────────────────────────────────
// ═══════════════════════════════════════════════════════════════════════════════

class RFAnalyzerProApp final : public hackos::HackOSApp
{
public:
    RFAnalyzerProApp()
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
        waterfallView_ = static_cast<RFWaterfallView *>(
            ctx().alloc(sizeof(RFWaterfallView)));
        if (waterfallView_ != nullptr)
        {
            new (waterfallView_) RFWaterfallView();
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

        sceneManager_ = new (std::nothrow) SceneManager(
            handlers, SCENE_COUNT, this);
        if (sceneManager_ != nullptr)
        {
            sceneManager_->navigateTo(SCENE_WATERFALL);
        }

        // Configure RF RX pin for fast digital reads
        pinMode(PIN_RF_RX, INPUT);

        needsRedraw_ = true;
        ESP_LOGI(TAG_RFA, "RFAnalyzerPro started (zoom=%s)",
                 ZOOM_LABELS[DEFAULT_ZOOM]);
    }

    void on_event(Event *event) override
    {
        if (event->type != EventType::EVT_INPUT)
        {
            return;
        }

        const auto input =
            static_cast<InputManager::InputEvent>(event->arg0);

        switch (input)
        {
        case InputManager::InputEvent::UP:
            // Zoom in (shorter inter-sample delay → finer detail)
            if (waterfallView_ != nullptr)
            {
                waterfallView_->zoomIn();
                needsRedraw_ = true;
            }
            break;

        case InputManager::InputEvent::DOWN:
            // Zoom out (longer inter-sample delay → wider view)
            if (waterfallView_ != nullptr)
            {
                waterfallView_->zoomOut();
                needsRedraw_ = true;
            }
            break;

        case InputManager::InputEvent::BUTTON_PRESS:
            // Toggle pause / resume
            if (waterfallView_ != nullptr)
            {
                waterfallView_->togglePause();
                needsRedraw_ = true;
            }
            break;

        case InputManager::InputEvent::LEFT:
            // Exit app
        {
            const Event evt{EventType::EVT_SYSTEM,
                            SYSTEM_EVENT_BACK, 0, nullptr};
            EventSystem::instance().postEvent(evt);
        }
        break;

        default:
            break;
        }
    }

    void on_free() override
    {
        viewDispatcher_.removeView(VIEW_WATERFALL);

        if (waterfallView_ != nullptr)
        {
            waterfallView_->~RFWaterfallView();
        }

        delete sceneManager_;
        sceneManager_ = nullptr;

        ESP_LOGI(TAG_RFA, "RFAnalyzerPro freed");
    }

    void on_update() override
    {
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

        DisplayManager &disp = DisplayManager::instance();
        disp.clear();
        statusBar_.draw();

        Canvas canvas;
        canvas.clear();
        viewDispatcher_.draw(&canvas);

        // Bulk-copy canvas buffer to display front buffer (DMA-style)
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
    RFWaterfallView *waterfallView_;
    SceneManager *sceneManager_;
    ViewDispatcher viewDispatcher_;
    StatusBar statusBar_;
    bool needsRedraw_;

    // ── Scene 0: Waterfall ──────────────────────────────────────────────

    static void sceneWaterfallEnter(void *context)
    {
        auto *app = static_cast<RFAnalyzerProApp *>(context);
        app->viewDispatcher().switchToView(VIEW_WATERFALL);
        app->requestRedraw();
    }

    static bool sceneWaterfallEvent(void *context, uint32_t eventId)
    {
        if (eventId == EVENT_BACK)
        {
            (void)context;
            const Event evt{EventType::EVT_SYSTEM,
                            SYSTEM_EVENT_BACK, 0, nullptr};
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

AppBase *createRFAnalyzerProApp()
{
    return new (std::nothrow) RFAnalyzerProApp();
}
