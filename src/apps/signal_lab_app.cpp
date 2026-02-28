/**
 * @file signal_lab_app.cpp
 * @brief SignalLab – Protocol Analyzer & Waterfall for 433 MHz RF.
 *
 * Combines three analysis tools in a single application:
 *
 *  1. **Waterfall Visualizer** – scrolling cascade display where each pixel
 *     row represents signal intensity at a point in time.  Uses high-speed
 *     ADC sampling of PIN_RF_RX with dithered intensity levels.
 *
 *  2. **Protocol Decoder** – captures edge timings via GPIO ISR and matches
 *     against common OOK protocols (Princeton, EV1527, HT6P20B).  Displays
 *     decoded device ID and function bits in real time.
 *
 *  3. **Pulse Width Analysis** – measures and graphically displays the
 *     Sync pulse, Pulse-High and Pulse-Low durations in microseconds.
 *     Essential for manually cloning unknown remotes.
 *
 * Navigation: UP/DOWN to scroll the menu, CENTER to select, LEFT to go back.
 */

#include "apps/signal_lab_app.h"

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

static constexpr const char *TAG_SL = "SignalLab";

/// Display geometry
static constexpr int16_t DISPLAY_W  = Canvas::WIDTH;   // 128
static constexpr int16_t DISPLAY_H  = Canvas::HEIGHT;  // 64

/// Header height (pixels) reserved for the title bar
static constexpr int16_t HEADER_H = 10;

/// Waterfall area height (pixels)
static constexpr int16_t WF_H = DISPLAY_H - HEADER_H; // 54

/// Samples per waterfall row (one per column)
static constexpr size_t SAMPLES_PER_ROW = 128U;

/// ADC resolution (12-bit on ESP32)
static constexpr uint16_t ADC_MAX = 4095U;

/// Number of intensity levels for dither patterns
static constexpr uint8_t INTENSITY_LEVELS = 4U;

/// High-speed sampling interval (µs)
static constexpr uint32_t SAMPLE_INTERVAL_US = 10U;

/// XP cooldown between awards (ms)
static constexpr uint32_t XP_COOLDOWN_MS = 10000U;

/// Packed row width for 2-bit intensity buffer: 128 columns × 2 bits / 8
static constexpr size_t WF_ROW_BYTES = (SAMPLES_PER_ROW * 2U + 7U) / 8U; // 32

// ── Protocol decoder constants ──────────────────────────────────────────────

/// Maximum number of pulse timings captured for protocol decoding
static constexpr size_t MAX_PULSES = 256U;

/// Tolerance percentage for pulse matching (±25%)
static constexpr uint32_t PULSE_TOLERANCE_PCT = 25U;

/// Minimum number of pulses for a valid frame
static constexpr size_t MIN_FRAME_PULSES = 8U;

/// Protocol IDs
enum ProtocolId : uint8_t
{
    PROTO_UNKNOWN   = 0U,
    PROTO_PRINCETON = 1U,
    PROTO_EV1527    = 2U,
    PROTO_HT6P20B   = 3U,
};

/// Protocol definition: timing ratios relative to a base pulse width
struct ProtocolDef
{
    const char *name;
    uint8_t     id;
    uint8_t     bitCount;       ///< Number of data bits
    uint8_t     syncHighRatio;  ///< Sync HIGH duration / base
    uint8_t     syncLowRatio;   ///< Sync LOW  duration / base
    uint8_t     zeroHighRatio;  ///< Bit-0 HIGH duration / base
    uint8_t     zeroLowRatio;   ///< Bit-0 LOW  duration / base
    uint8_t     oneHighRatio;   ///< Bit-1 HIGH duration / base
    uint8_t     oneLowRatio;    ///< Bit-1 LOW  duration / base
};

/// Supported protocol table
static constexpr ProtocolDef PROTOCOLS[] = {
    // Princeton PT2262: sync 1:31, bit0 1:3, bit1 3:1, 24 bits
    {"Princeton", PROTO_PRINCETON, 24U, 1U, 31U, 1U, 3U, 3U, 1U},
    // EV1527: sync 1:31, bit0 1:3, bit1 3:1, 24 bits (same timing)
    {"EV1527",    PROTO_EV1527,    24U, 1U, 31U, 1U, 3U, 3U, 1U},
    // HT6P20B: sync 1:23, bit0 1:2, bit1 2:1, 28 bits
    {"HT6P20B",   PROTO_HT6P20B,   28U, 1U, 23U, 1U, 2U, 2U, 1U},
};

static constexpr size_t PROTO_COUNT = sizeof(PROTOCOLS) / sizeof(PROTOCOLS[0]);

// ── ISR-shared pulse capture state ──────────────────────────────────────────

/// Circular buffer for edge timings captured by the GPIO ISR.
/// Positive values = HIGH duration (µs), negative = LOW duration (µs).
static volatile int32_t  s_pulseBuf[MAX_PULSES];
static volatile size_t   s_pulseHead  = 0U;
static volatile size_t   s_pulseCount = 0U;
static volatile int64_t  s_lastEdgeUs = 0;
static volatile bool     s_isrActive  = false;

/// GPIO ISR handler: captures edge timing into the circular buffer.
static void IRAM_ATTR rfEdgeISR(void * /*arg*/)
{
    const int64_t now = esp_timer_get_time();
    if (s_lastEdgeUs == 0)
    {
        s_lastEdgeUs = now;
        return;
    }

    const int32_t duration = static_cast<int32_t>(now - s_lastEdgeUs);
    s_lastEdgeUs = now;

    // Determine if this edge is rising (signal went HIGH) or falling
    const int level = gpio_get_level(static_cast<gpio_num_t>(PIN_RF_RX));
    // After a rising edge the pin is HIGH → previous period was LOW
    const int32_t signedDur = (level != 0) ? -duration : duration;

    const size_t idx = s_pulseHead;
    s_pulseBuf[idx] = signedDur;
    s_pulseHead = (idx + 1U) % MAX_PULSES;
    if (s_pulseCount < MAX_PULSES)
    {
        ++s_pulseCount;
    }
}

// ── Scene / View IDs ────────────────────────────────────────────────────────

enum SceneId : uint32_t
{
    SCENE_MENU      = 0U,
    SCENE_WATERFALL = 1U,
    SCENE_DECODER   = 2U,
    SCENE_PULSE     = 3U,
    SCENE_COUNT     = 4U,
};

enum ViewId : uint32_t
{
    VIEW_MENU      = 0U,
    VIEW_WATERFALL = 1U,
    VIEW_DECODER   = 2U,
    VIEW_PULSE     = 3U,
};

enum AppEvent : uint32_t
{
    EVENT_BACK        = 100U,
    EVENT_WATERFALL   = 101U,
    EVENT_DECODER     = 102U,
    EVENT_PULSE       = 103U,
};

// ── Helper: pulse matching with tolerance ───────────────────────────────────

static bool pulsesMatch(uint32_t measured, uint32_t expected)
{
    if (expected == 0U)
    {
        return false;
    }
    const uint32_t tolerance = expected * PULSE_TOLERANCE_PCT / 100U;
    return (measured >= expected - tolerance) &&
           (measured <= expected + tolerance);
}

// ═════════════════════════════════════════════════════════════════════════════
// ── MenuView ────────────────────────────────────────────────────────────────
// ═════════════════════════════════════════════════════════════════════════════

class SLMenuView final : public View
{
public:
    SLMenuView() : sel_(0) {}

    void draw(Canvas *canvas) override
    {
        canvas->drawStr(2, 7, "SignalLab 433MHz");
        canvas->drawLine(0, HEADER_H - 1, DISPLAY_W - 1, HEADER_H - 1);

        static const char *items[] = {
            "Waterfall",
            "Protocol Decoder",
            "Pulse Analysis",
        };
        static constexpr size_t ITEM_COUNT = 3U;

        for (size_t i = 0U; i < ITEM_COUNT; ++i)
        {
            const int16_t y = HEADER_H + 4 + static_cast<int16_t>(i) * 14;
            if (i == sel_)
            {
                canvas->fillRect(0, y - 2, DISPLAY_W, 13);
                canvas->drawStr(4, y + 7, items[i], 0); // inverted
            }
            else
            {
                canvas->drawStr(4, y + 7, items[i]);
            }
        }
    }

    bool input(InputEvent * /*event*/) override { return false; }

    void moveUp()   { if (sel_ > 0U) { --sel_; } }
    void moveDown() { if (sel_ < 2U) { ++sel_; } }
    size_t selected() const { return sel_; }

private:
    size_t sel_;
};

// ═════════════════════════════════════════════════════════════════════════════
// ── WaterfallView ───────────────────────────────────────────────────────────
// ═════════════════════════════════════════════════════════════════════════════

class SLWaterfallView final : public View
{
public:
    SLWaterfallView()
        : peakCol_(0),
          peakVal_(0)
    {
        std::memset(wfBuf_, 0, sizeof(wfBuf_));
    }

    /// Sample one row of ADC data from PIN_RF_RX.
    void sampleRow()
    {
        uint16_t peakV = 0U;
        uint8_t  peakC = 0U;

        for (size_t i = 0U; i < SAMPLES_PER_ROW; ++i)
        {
            const uint16_t raw =
                static_cast<uint16_t>(analogRead(PIN_RF_RX));
            const uint8_t q = quantise(raw);
            setIntensity(0U, i, q);

            if (raw > peakV)
            {
                peakV = raw;
                peakC = static_cast<uint8_t>(i);
            }
            delayMicroseconds(SAMPLE_INTERVAL_US);
        }

        peakCol_ = peakC;
        peakVal_ = peakV;

        // Scroll waterfall down (newest row at top)
        scrollDown();
    }

    void draw(Canvas *canvas) override
    {
        canvas->drawStr(2, 7, "Waterfall 433MHz");
        canvas->drawLine(0, HEADER_H - 1, DISPLAY_W - 1, HEADER_H - 1);

        drawWaterfall(canvas);

        // Peak marker
        if (peakVal_ > ADC_MAX / 10U)
        {
            const int16_t px = static_cast<int16_t>(peakCol_);
            canvas->drawPixel(px, HEADER_H);
            if (px > 0) { canvas->drawPixel(px - 1, HEADER_H + 1); }
            if (px < DISPLAY_W - 1) { canvas->drawPixel(px + 1, HEADER_H + 1); }
        }

        // Peak value text
        char buf[18];
        std::snprintf(buf, sizeof(buf), "Pk:%u", peakVal_);
        canvas->drawStr(90, DISPLAY_H - 1, buf);
    }

    bool input(InputEvent * /*event*/) override { return false; }

private:
    uint8_t wfBuf_[WF_H][WF_ROW_BYTES];
    uint8_t peakCol_;
    uint16_t peakVal_;

    static uint8_t quantise(uint16_t raw)
    {
        if (raw < ADC_MAX / 4U)    { return 0U; }
        if (raw < ADC_MAX / 2U)    { return 1U; }
        if (raw < (ADC_MAX * 3U) / 4U) { return 2U; }
        return 3U;
    }

    uint8_t getIntensity(size_t row, size_t col) const
    {
        const size_t byteIdx = (col * 2U) / 8U;
        const uint8_t shift  = static_cast<uint8_t>((col * 2U) % 8U);
        return (wfBuf_[row][byteIdx] >> shift) & 0x03U;
    }

    void setIntensity(size_t row, size_t col, uint8_t val)
    {
        const size_t byteIdx = (col * 2U) / 8U;
        const uint8_t shift  = static_cast<uint8_t>((col * 2U) % 8U);
        wfBuf_[row][byteIdx] = static_cast<uint8_t>(
            (wfBuf_[row][byteIdx] & ~(0x03U << shift)) |
            ((val & 0x03U) << shift));
    }

    void scrollDown()
    {
        for (int r = WF_H - 1; r > 0; --r)
        {
            std::memcpy(wfBuf_[r], wfBuf_[r - 1], WF_ROW_BYTES);
        }
        std::memset(wfBuf_[0], 0, WF_ROW_BYTES);
    }

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
                case 1U: lit = ((r + c) % 4 == 0); break;
                case 2U: lit = ((r + c) % 2 == 0); break;
                case 3U: lit = true; break;
                default: break;
                }
                if (lit) { canvas->drawPixel(c, y); }
            }
        }
    }
};

// ═════════════════════════════════════════════════════════════════════════════
// ── DecoderView – Protocol Decoder ──────────────────────────────────────────
// ═════════════════════════════════════════════════════════════════════════════

class SLDecoderView final : public View
{
public:
    SLDecoderView()
        : detectedProto_(PROTO_UNKNOWN),
          decodedCode_(0U),
          decodedBits_(0U),
          lastDecodeMs_(0U)
    {
    }

    /// Copy ISR buffer and attempt protocol decoding.
    void processCapture()
    {
        // Snapshot the volatile ISR buffer
        noInterrupts();
        const size_t count = s_pulseCount;
        int32_t localBuf[MAX_PULSES];
        size_t head = s_pulseHead;
        for (size_t i = 0U; i < count; ++i)
        {
            const size_t idx = (head + MAX_PULSES - count + i) % MAX_PULSES;
            localBuf[i] = s_pulseBuf[idx];
        }
        interrupts();

        if (count < MIN_FRAME_PULSES)
        {
            return;
        }

        // Try each protocol
        for (size_t p = 0U; p < PROTO_COUNT; ++p)
        {
            if (tryDecode(PROTOCOLS[p], localBuf, count))
            {
                detectedProto_ = PROTOCOLS[p].id;
                const uint32_t now = static_cast<uint32_t>(millis());
                if (now - lastDecodeMs_ > 500U)
                {
                    lastDecodeMs_ = now;
                }
                return;
            }
        }
    }

    void draw(Canvas *canvas) override
    {
        canvas->drawStr(2, 7, "Protocol Decoder");
        canvas->drawLine(0, HEADER_H - 1, DISPLAY_W - 1, HEADER_H - 1);

        if (detectedProto_ != PROTO_UNKNOWN)
        {
            // Protocol name
            const char *name = "Unknown";
            for (size_t p = 0U; p < PROTO_COUNT; ++p)
            {
                if (PROTOCOLS[p].id == detectedProto_)
                {
                    name = PROTOCOLS[p].name;
                    break;
                }
            }
            char line1[32];
            std::snprintf(line1, sizeof(line1), "Proto: %s", name);
            canvas->drawStr(2, HEADER_H + 10, line1);

            // Decoded ID (hex)
            char line2[32];
            std::snprintf(line2, sizeof(line2), "ID: 0x%06lX",
                          static_cast<unsigned long>(decodedCode_));
            canvas->drawStr(2, HEADER_H + 22, line2);

            // Decoded bits
            char line3[32];
            std::snprintf(line3, sizeof(line3), "Bits: %u",
                          static_cast<unsigned>(decodedBits_));
            canvas->drawStr(2, HEADER_H + 34, line3);

            // Function bits (last 4 bits)
            const uint8_t funcBits =
                static_cast<uint8_t>(decodedCode_ & 0x0FU);
            char line4[32];
            std::snprintf(line4, sizeof(line4), "Func: 0x%X (%u)",
                          funcBits, funcBits);
            canvas->drawStr(2, HEADER_H + 46, line4);
        }
        else
        {
            canvas->drawStr(2, HEADER_H + 16, "Listening...");
            canvas->drawStr(2, HEADER_H + 30, "Waiting for signal");

            // Show pulse count for feedback
            char cntTxt[24];
            std::snprintf(cntTxt, sizeof(cntTxt), "Pulses: %u",
                          static_cast<unsigned>(s_pulseCount));
            canvas->drawStr(2, HEADER_H + 44, cntTxt);
        }
    }

    bool input(InputEvent * /*event*/) override { return false; }

    void reset()
    {
        detectedProto_ = PROTO_UNKNOWN;
        decodedCode_   = 0U;
        decodedBits_   = 0U;
    }

private:
    uint8_t  detectedProto_;
    uint32_t decodedCode_;
    uint8_t  decodedBits_;
    uint32_t lastDecodeMs_;

    /**
     * @brief Attempt to decode a pulse train using a specific protocol.
     *
     * Searches the buffer for a sync pulse matching the protocol's
     * sync ratio, then decodes subsequent bit pairs.
     */
    bool tryDecode(const ProtocolDef &proto,
                   const int32_t *buf, size_t count)
    {
        // Need at least sync + (bitCount * 2) pulses
        const size_t needed = 2U + static_cast<size_t>(proto.bitCount) * 2U;
        if (count < needed)
        {
            return false;
        }

        // Scan for sync pulse pair
        for (size_t i = 0U; i + needed <= count; ++i)
        {
            const uint32_t h = static_cast<uint32_t>(
                buf[i] > 0 ? buf[i] : -buf[i]);
            const uint32_t l = static_cast<uint32_t>(
                buf[i + 1U] > 0 ? buf[i + 1U] : -buf[i + 1U]);

            if (h == 0U || l == 0U)
            {
                continue;
            }

            // Estimate base pulse width from sync
            const uint32_t baseFromH = h / proto.syncHighRatio;
            const uint32_t baseFromL = l / proto.syncLowRatio;
            const uint32_t base = (baseFromH + baseFromL) / 2U;

            if (base < 50U || base > 2000U)
            {
                continue; // unreasonable base pulse width
            }

            // Verify sync ratio match
            if (!pulsesMatch(h, base * proto.syncHighRatio) ||
                !pulsesMatch(l, base * proto.syncLowRatio))
            {
                continue;
            }

            // Attempt to decode bits
            uint32_t code = 0U;
            uint8_t  bitsDecoded = 0U;
            bool valid = true;

            for (uint8_t b = 0U; b < proto.bitCount; ++b)
            {
                const size_t bi = i + 2U + static_cast<size_t>(b) * 2U;
                if (bi + 1U >= count)
                {
                    valid = false;
                    break;
                }

                const uint32_t bh = static_cast<uint32_t>(
                    buf[bi] > 0 ? buf[bi] : -buf[bi]);
                const uint32_t bl = static_cast<uint32_t>(
                    buf[bi + 1U] > 0 ? buf[bi + 1U] : -buf[bi + 1U]);

                // Check for bit-1 pattern
                if (pulsesMatch(bh, base * proto.oneHighRatio) &&
                    pulsesMatch(bl, base * proto.oneLowRatio))
                {
                    code = (code << 1U) | 1U;
                    ++bitsDecoded;
                }
                // Check for bit-0 pattern
                else if (pulsesMatch(bh, base * proto.zeroHighRatio) &&
                         pulsesMatch(bl, base * proto.zeroLowRatio))
                {
                    code = (code << 1U);
                    ++bitsDecoded;
                }
                else
                {
                    valid = false;
                    break;
                }
            }

            if (valid && bitsDecoded == proto.bitCount)
            {
                decodedCode_ = code;
                decodedBits_ = bitsDecoded;
                return true;
            }
        }

        return false;
    }
};

// ═════════════════════════════════════════════════════════════════════════════
// ── PulseView – Pulse Width Analysis ────────────────────────────────────────
// ═════════════════════════════════════════════════════════════════════════════

class SLPulseView final : public View
{
public:
    SLPulseView()
        : syncUs_(0U),
          highUs_(0U),
          lowUs_(0U),
          pulseCount_(0U)
    {
    }

    /// Analyse the ISR pulse buffer and extract timing statistics.
    void analyse()
    {
        noInterrupts();
        const size_t count = s_pulseCount;
        int32_t localBuf[MAX_PULSES];
        const size_t head = s_pulseHead;
        for (size_t i = 0U; i < count; ++i)
        {
            const size_t idx = (head + MAX_PULSES - count + i) % MAX_PULSES;
            localBuf[i] = s_pulseBuf[idx];
        }
        interrupts();

        if (count < 4U)
        {
            return;
        }

        pulseCount_ = static_cast<uint16_t>(count);

        // Find the longest LOW duration as the likely sync pulse
        uint32_t maxLow = 0U;
        for (size_t i = 0U; i < count; ++i)
        {
            if (localBuf[i] < 0)
            {
                const uint32_t dur = static_cast<uint32_t>(-localBuf[i]);
                if (dur > maxLow) { maxLow = dur; }
            }
        }
        syncUs_ = maxLow;

        // Average HIGH and LOW durations (excluding sync)
        uint64_t sumH = 0U;
        uint32_t cntH = 0U;
        uint64_t sumL = 0U;
        uint32_t cntL = 0U;

        for (size_t i = 0U; i < count; ++i)
        {
            const uint32_t dur = static_cast<uint32_t>(
                localBuf[i] > 0 ? localBuf[i] : -localBuf[i]);

            // Exclude sync-length pulses from averages
            if (syncUs_ > 0U && dur > syncUs_ / 2U)
            {
                continue;
            }

            if (localBuf[i] > 0)
            {
                sumH += dur;
                ++cntH;
            }
            else
            {
                sumL += dur;
                ++cntL;
            }
        }

        highUs_ = (cntH > 0U) ? static_cast<uint32_t>(sumH / cntH) : 0U;
        lowUs_  = (cntL > 0U) ? static_cast<uint32_t>(sumL / cntL) : 0U;
    }

    void draw(Canvas *canvas) override
    {
        canvas->drawStr(2, 7, "Pulse Analysis");
        canvas->drawLine(0, HEADER_H - 1, DISPLAY_W - 1, HEADER_H - 1);

        // Sync duration
        char buf[30];
        std::snprintf(buf, sizeof(buf), "Sync : %lu us",
                      static_cast<unsigned long>(syncUs_));
        canvas->drawStr(2, HEADER_H + 10, buf);

        // Pulse HIGH average
        std::snprintf(buf, sizeof(buf), "High : %lu us",
                      static_cast<unsigned long>(highUs_));
        canvas->drawStr(2, HEADER_H + 20, buf);

        // Pulse LOW average
        std::snprintf(buf, sizeof(buf), "Low  : %lu us",
                      static_cast<unsigned long>(lowUs_));
        canvas->drawStr(2, HEADER_H + 30, buf);

        // Pulse count
        std::snprintf(buf, sizeof(buf), "Count: %u",
                      static_cast<unsigned>(pulseCount_));
        canvas->drawStr(2, HEADER_H + 40, buf);

        // Graphical bar representation (bottom area)
        drawBars(canvas);
    }

    bool input(InputEvent * /*event*/) override { return false; }

private:
    uint32_t syncUs_;
    uint32_t highUs_;
    uint32_t lowUs_;
    uint16_t pulseCount_;

    /// Draw proportional horizontal bars for Sync, High, Low.
    void drawBars(Canvas *canvas) const
    {
        const int16_t barY    = DISPLAY_H - 9;
        const int16_t barMaxW = DISPLAY_W - 4;

        // Find maximum for scaling
        uint32_t maxVal = syncUs_;
        if (highUs_ > maxVal) { maxVal = highUs_; }
        if (lowUs_  > maxVal) { maxVal = lowUs_; }
        if (maxVal == 0U) { return; }

        // Sync bar
        const int16_t sW = static_cast<int16_t>(
            static_cast<uint32_t>(barMaxW) * syncUs_ / maxVal);
        if (sW > 0) { canvas->fillRect(2, barY, sW, 2); }

        // High bar
        const int16_t hW = static_cast<int16_t>(
            static_cast<uint32_t>(barMaxW) * highUs_ / maxVal);
        if (hW > 0) { canvas->fillRect(2, barY + 3, hW, 2); }

        // Low bar
        const int16_t lW = static_cast<int16_t>(
            static_cast<uint32_t>(barMaxW) * lowUs_ / maxVal);
        if (lW > 0) { canvas->fillRect(2, barY + 6, lW, 2); }
    }
};

// ═════════════════════════════════════════════════════════════════════════════
// ── SignalLabApp ─────────────────────────────────────────────────────────────
// ═════════════════════════════════════════════════════════════════════════════

class SignalLabApp final : public hackos::HackOSApp
{
public:
    SignalLabApp()
        : menuView_(nullptr),
          waterfallView_(nullptr),
          decoderView_(nullptr),
          pulseView_(nullptr),
          sceneManager_(nullptr),
          viewDispatcher_(),
          statusBar_(0, 0, 128, 8),
          needsRedraw_(true),
          activeScene_(SCENE_MENU),
          xpAwarded_(false),
          xpCooldownMs_(0U)
    {
    }

    // ── HackOSApp lifecycle ──────────────────────────────────────────────

    void on_alloc() override
    {
        menuView_ = static_cast<SLMenuView *>(
            ctx().alloc(sizeof(SLMenuView)));
        if (menuView_ != nullptr) { new (menuView_) SLMenuView(); }

        waterfallView_ = static_cast<SLWaterfallView *>(
            ctx().alloc(sizeof(SLWaterfallView)));
        if (waterfallView_ != nullptr) { new (waterfallView_) SLWaterfallView(); }

        decoderView_ = static_cast<SLDecoderView *>(
            ctx().alloc(sizeof(SLDecoderView)));
        if (decoderView_ != nullptr) { new (decoderView_) SLDecoderView(); }

        pulseView_ = static_cast<SLPulseView *>(
            ctx().alloc(sizeof(SLPulseView)));
        if (pulseView_ != nullptr) { new (pulseView_) SLPulseView(); }
    }

    void on_start() override
    {
        statusBar_.setConnectivity(false, false);
        statusBar_.setBatteryLevel(100U);
        statusBar_.setTime(0U, 0U);

        if (menuView_      != nullptr) { viewDispatcher_.addView(VIEW_MENU, menuView_); }
        if (waterfallView_ != nullptr) { viewDispatcher_.addView(VIEW_WATERFALL, waterfallView_); }
        if (decoderView_   != nullptr) { viewDispatcher_.addView(VIEW_DECODER, decoderView_); }
        if (pulseView_     != nullptr) { viewDispatcher_.addView(VIEW_PULSE, pulseView_); }

        static const SceneHandler handlers[SCENE_COUNT] = {
            {sceneMenuEnter,      sceneMenuEvent,      sceneMenuExit},
            {sceneWaterfallEnter, sceneWaterfallEvent, sceneWaterfallExit},
            {sceneDecoderEnter,   sceneDecoderEvent,   sceneDecoderExit},
            {scenePulseEnter,     scenePulseEvent,     scenePulseExit},
        };

        sceneManager_ = new (std::nothrow) SceneManager(
            handlers, SCENE_COUNT, this);
        if (sceneManager_ != nullptr)
        {
            sceneManager_->navigateTo(SCENE_MENU);
        }

        // Configure RF RX pin for ADC and digital reads
        analogReadResolution(12);
        analogSetPinAttenuation(PIN_RF_RX, ADC_11db);
        pinMode(PIN_RF_RX, INPUT);

        // Install GPIO ISR for pulse capture (decoder + pulse views)
        gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
        gpio_set_intr_type(
            static_cast<gpio_num_t>(PIN_RF_RX), GPIO_INTR_ANYEDGE);
        gpio_isr_handler_add(
            static_cast<gpio_num_t>(PIN_RF_RX), rfEdgeISR, nullptr);
        s_isrActive = true;
        s_lastEdgeUs = 0;
        s_pulseHead  = 0U;
        s_pulseCount = 0U;

        needsRedraw_ = true;
        ESP_LOGI(TAG_SL, "SignalLab started");
    }

    void on_event(Event *event) override
    {
        if (event->type != EventType::EVT_INPUT)
        {
            return;
        }

        const auto input =
            static_cast<InputManager::InputEvent>(event->arg0);

        switch (activeScene_)
        {
        case SCENE_MENU:
            handleMenuInput(input);
            break;
        case SCENE_WATERFALL:
        case SCENE_DECODER:
        case SCENE_PULSE:
            handleSubviewInput(input);
            break;
        default:
            break;
        }
    }

    void on_free() override
    {
        // Remove ISR
        if (s_isrActive)
        {
            gpio_isr_handler_remove(static_cast<gpio_num_t>(PIN_RF_RX));
            s_isrActive = false;
        }

        viewDispatcher_.removeView(VIEW_MENU);
        viewDispatcher_.removeView(VIEW_WATERFALL);
        viewDispatcher_.removeView(VIEW_DECODER);
        viewDispatcher_.removeView(VIEW_PULSE);

        if (menuView_      != nullptr) { menuView_->~SLMenuView(); }
        if (waterfallView_ != nullptr) { waterfallView_->~SLWaterfallView(); }
        if (decoderView_   != nullptr) { decoderView_->~SLDecoderView(); }
        if (pulseView_     != nullptr) { pulseView_->~SLPulseView(); }

        delete sceneManager_;
        sceneManager_ = nullptr;

        ESP_LOGI(TAG_SL, "SignalLab freed");
    }

    void on_update() override
    {
        switch (activeScene_)
        {
        case SCENE_WATERFALL:
            if (waterfallView_ != nullptr)
            {
                waterfallView_->sampleRow();
                needsRedraw_ = true;
            }
            break;

        case SCENE_DECODER:
            if (decoderView_ != nullptr)
            {
                decoderView_->processCapture();
                needsRedraw_ = true;
            }
            break;

        case SCENE_PULSE:
            if (pulseView_ != nullptr)
            {
                pulseView_->analyse();
                needsRedraw_ = true;
            }
            break;

        default:
            break;
        }

        // XP award on sustained use
        const uint32_t now = static_cast<uint32_t>(millis());
        if (activeScene_ != SCENE_MENU &&
            (!xpAwarded_ || (now - xpCooldownMs_ > XP_COOLDOWN_MS)))
        {
            const Event xpEvt{EventType::EVT_XP_EARNED,
                              XP_SIGNAL_ANALYZE, 0, nullptr};
            EventSystem::instance().postEvent(xpEvt);
            xpAwarded_ = true;
            xpCooldownMs_ = now;
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
    void setActiveScene(SceneId s) { activeScene_ = s; }

private:
    SLMenuView       *menuView_;
    SLWaterfallView  *waterfallView_;
    SLDecoderView    *decoderView_;
    SLPulseView      *pulseView_;
    SceneManager     *sceneManager_;
    ViewDispatcher    viewDispatcher_;
    StatusBar         statusBar_;
    bool              needsRedraw_;
    SceneId           activeScene_;
    bool              xpAwarded_;
    uint32_t          xpCooldownMs_;

    // ── Input handlers ──────────────────────────────────────────────────

    void handleMenuInput(InputManager::InputEvent input)
    {
        switch (input)
        {
        case InputManager::InputEvent::UP:
            if (menuView_ != nullptr) { menuView_->moveUp(); needsRedraw_ = true; }
            break;
        case InputManager::InputEvent::DOWN:
            if (menuView_ != nullptr) { menuView_->moveDown(); needsRedraw_ = true; }
            break;
        case InputManager::InputEvent::BUTTON_PRESS:
            if (menuView_ != nullptr)
            {
                switch (menuView_->selected())
                {
                case 0U:
                    if (sceneManager_ != nullptr)
                    {
                        sceneManager_->navigateTo(SCENE_WATERFALL);
                    }
                    break;
                case 1U:
                    if (sceneManager_ != nullptr)
                    {
                        // Reset decoder state when entering
                        if (decoderView_ != nullptr) { decoderView_->reset(); }
                        s_pulseHead = 0U;
                        s_pulseCount = 0U;
                        s_lastEdgeUs = 0;
                        sceneManager_->navigateTo(SCENE_DECODER);
                    }
                    break;
                case 2U:
                    if (sceneManager_ != nullptr)
                    {
                        s_pulseHead = 0U;
                        s_pulseCount = 0U;
                        s_lastEdgeUs = 0;
                        sceneManager_->navigateTo(SCENE_PULSE);
                    }
                    break;
                default:
                    break;
                }
            }
            break;
        case InputManager::InputEvent::LEFT:
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

    void handleSubviewInput(InputManager::InputEvent input)
    {
        if (input == InputManager::InputEvent::LEFT)
        {
            // Go back to menu
            if (sceneManager_ != nullptr)
            {
                sceneManager_->navigateTo(SCENE_MENU);
            }
        }
    }

    // ── Scene 0: Menu ───────────────────────────────────────────────────

    static void sceneMenuEnter(void *context)
    {
        auto *app = static_cast<SignalLabApp *>(context);
        app->setActiveScene(SCENE_MENU);
        app->viewDispatcher().switchToView(VIEW_MENU);
        app->requestRedraw();
    }

    static bool sceneMenuEvent(void * /*context*/, uint32_t /*eventId*/)
    {
        return false;
    }

    static void sceneMenuExit(void * /*context*/) {}

    // ── Scene 1: Waterfall ──────────────────────────────────────────────

    static void sceneWaterfallEnter(void *context)
    {
        auto *app = static_cast<SignalLabApp *>(context);
        app->setActiveScene(SCENE_WATERFALL);
        app->viewDispatcher().switchToView(VIEW_WATERFALL);
        app->requestRedraw();
    }

    static bool sceneWaterfallEvent(void * /*context*/, uint32_t eventId)
    {
        if (eventId == EVENT_BACK)
        {
            const Event evt{EventType::EVT_SYSTEM,
                            SYSTEM_EVENT_BACK, 0, nullptr};
            EventSystem::instance().postEvent(evt);
            return true;
        }
        return false;
    }

    static void sceneWaterfallExit(void * /*context*/) {}

    // ── Scene 2: Decoder ────────────────────────────────────────────────

    static void sceneDecoderEnter(void *context)
    {
        auto *app = static_cast<SignalLabApp *>(context);
        app->setActiveScene(SCENE_DECODER);
        app->viewDispatcher().switchToView(VIEW_DECODER);
        app->requestRedraw();
    }

    static bool sceneDecoderEvent(void * /*context*/, uint32_t eventId)
    {
        if (eventId == EVENT_BACK)
        {
            const Event evt{EventType::EVT_SYSTEM,
                            SYSTEM_EVENT_BACK, 0, nullptr};
            EventSystem::instance().postEvent(evt);
            return true;
        }
        return false;
    }

    static void sceneDecoderExit(void * /*context*/) {}

    // ── Scene 3: Pulse ──────────────────────────────────────────────────

    static void scenePulseEnter(void *context)
    {
        auto *app = static_cast<SignalLabApp *>(context);
        app->setActiveScene(SCENE_PULSE);
        app->viewDispatcher().switchToView(VIEW_PULSE);
        app->requestRedraw();
    }

    static bool scenePulseEvent(void * /*context*/, uint32_t eventId)
    {
        if (eventId == EVENT_BACK)
        {
            const Event evt{EventType::EVT_SYSTEM,
                            SYSTEM_EVENT_BACK, 0, nullptr};
            EventSystem::instance().postEvent(evt);
            return true;
        }
        return false;
    }

    static void scenePulseExit(void * /*context*/) {}
};

} // namespace

// ── Factory function ─────────────────────────────────────────────────────────

AppBase *createSignalLabApp()
{
    return new (std::nothrow) SignalLabApp();
}
