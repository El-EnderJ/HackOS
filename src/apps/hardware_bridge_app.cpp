/**
 * @file hardware_bridge_app.cpp
 * @brief HardwareBridge – Logic Analyzer, Voltmeter & Signal Generator.
 *
 * Phase 28 deliverables:
 *  1. **UART/I2C/SPI Sniffer** – sniffs bus traffic on configurable GPIO
 *     pins.  Captured bytes are displayed on the OLED and streamed to the
 *     Remote Dashboard (Fase 17) via a Server-Sent Events endpoint
 *     (/api/hwbridge/live).
 *  2. **Digital Voltmeter** – reads the ESP32 ADC (0-3.3 V) and renders a
 *     live bar-graph on the OLED display.
 *  3. **Signal Generator** – outputs a configurable-frequency PWM square
 *     wave on a GPIO pin for probing actuators or simulating sensors.
 *
 * Navigation: UP/DOWN to scroll menu, CENTER to select, LEFT to go back.
 */

#include "apps/hardware_bridge_app.h"

#include <cstdio>
#include <cstring>
#include <new>

#include <Arduino.h>
#include <Wire.h>
#include <esp_log.h>
#include <esp_http_server.h>
#include <driver/ledc.h>

#include "config.h"
#include "core/event.h"
#include "core/event_system.h"
#include "core/experience_manager.h"
#include "hardware/display.h"
#include "hardware/input.h"

// ══════════════════════════════════════════════════════════════════════════════
// Anonymous namespace – all internal implementation
// ══════════════════════════════════════════════════════════════════════════════

namespace
{

// ── Constants ────────────────────────────────────────────────────────────────

static constexpr const char *TAG_HB = "HwBridge";

/// Display geometry
static constexpr int16_t DISPLAY_W = 128;
static constexpr int16_t DISPLAY_H = 64;
static constexpr int16_t HEADER_H  = 10;

/// ADC / Voltmeter
static constexpr uint16_t ADC_MAX         = 4095U;
static constexpr float    ADC_REF_VOLTAGE = 3.3f;
static constexpr size_t   VM_HISTORY_LEN  = 100U; ///< scrolling history
static constexpr uint32_t VM_SAMPLE_MS    = 50U;   ///< sample every 50 ms

/// Default HW-bridge pins (free GPIOs on ESP32 DevKit v1)
static constexpr uint8_t PIN_HB_UART_RX = 26U; ///< sniff UART RX
static constexpr uint8_t PIN_HB_UART_TX = 33U; ///< sniff UART TX
static constexpr uint8_t PIN_HB_SDA     = 13U; ///< I2C data sniff
static constexpr uint8_t PIN_HB_SCL     = 14U; ///< I2C clock sniff
static constexpr uint8_t PIN_HB_SPI_CLK = 12U; ///< SPI clock sniff
static constexpr uint8_t PIN_HB_SPI_MOSI = 2U; ///< SPI MOSI sniff
static constexpr uint8_t PIN_HB_SPI_MISO = 0U; ///< SPI MISO sniff (input-only ok)
static constexpr uint8_t PIN_HB_ADC     = 36U; ///< ADC input (VP, input-only)
static constexpr uint8_t PIN_HB_SIGGEN  = 33U; ///< PWM signal output

/// Sniffer ring buffer
static constexpr size_t SNIFF_BUF_SIZE = 512U;

/// Signal generator
static constexpr uint32_t SIGGEN_FREQ_MIN  = 1U;
static constexpr uint32_t SIGGEN_FREQ_MAX  = 500000U;
static constexpr uint8_t  SIGGEN_DUTY_BITS = 8U;

/// SSE streaming
static constexpr size_t   SSE_LINE_MAX = 256U;
static constexpr uint32_t SSE_INTERVAL_MS = 200U;
static constexpr int      SSE_MAX_SAMPLES = 150;

/// LEDC channel / timer for signal generator
static constexpr ledc_channel_t SIGGEN_LEDC_CH    = LEDC_CHANNEL_1;
static constexpr ledc_timer_t   SIGGEN_LEDC_TIMER = LEDC_TIMER_1;

/// XP cooldown
static constexpr uint32_t XP_COOLDOWN_MS = 30000U;

// ── Sniffer protocol enum ────────────────────────────────────────────────────

enum class SniffProto : uint8_t
{
    UART = 0U,
    I2C  = 1U,
    SPI  = 2U,
    COUNT = 3U
};

static const char *sniffProtoName(SniffProto p)
{
    switch (p)
    {
    case SniffProto::UART: return "UART";
    case SniffProto::I2C:  return "I2C";
    case SniffProto::SPI:  return "SPI";
    default:               return "???";
    }
}

// ── App view states ──────────────────────────────────────────────────────────

enum class HBView : uint8_t
{
    MENU       = 0U,
    SNIFFER    = 1U,
    VOLTMETER  = 2U,
    SIGGEN     = 3U
};

// ── Sniffer circular buffer ──────────────────────────────────────────────────

struct SniffRing
{
    uint8_t  data[SNIFF_BUF_SIZE];
    volatile size_t head;
    volatile size_t tail;

    void reset()
    {
        head = 0U;
        tail = 0U;
        std::memset(data, 0, sizeof(data));
    }

    bool push(uint8_t b)
    {
        const size_t next = (head + 1U) % SNIFF_BUF_SIZE;
        if (next == tail) return false;
        data[head] = b;
        head = next;
        return true;
    }

    bool pop(uint8_t &out)
    {
        if (tail == head) return false;
        out = data[tail];
        tail = (tail + 1U) % SNIFF_BUF_SIZE;
        return true;
    }

    size_t available() const
    {
        const size_t h = head;
        const size_t t = tail;
        return (h >= t) ? (h - t) : (SNIFF_BUF_SIZE - t + h);
    }
};

// ── Shared state ─────────────────────────────────────────────────────────────

static SniffRing         g_sniffRing;
static volatile bool     g_sniffActive   = false;
static volatile uint32_t g_sniffBytes    = 0U;
static SniffProto        g_sniffProto    = SniffProto::UART;

/// UART sniffer uses HardwareSerial(2)
static HardwareSerial    g_sniffSerial(2);

// ── SSE shared buffer for dashboard passthrough ──────────────────────────────
// Latest hex-dump line ready for SSE streaming
static char              g_sseLineBuffer[SSE_LINE_MAX] = {};
static volatile bool     g_sseNewData = false;

// ── Voltmeter SSE data ──────────────────────────────────────────────────────
static volatile float    g_lastVoltage = 0.0f;

// ══════════════════════════════════════════════════════════════════════════════
// HardwareBridgeApp class
// ══════════════════════════════════════════════════════════════════════════════

class HardwareBridgeApp : public AppBase
{
public:
    HardwareBridgeApp() = default;
    ~HardwareBridgeApp() override = default;

    // ── AppBase lifecycle ────────────────────────────────────────────────

    void onSetup() override
    {
        view_       = HBView::MENU;
        menuSel_    = 0U;
        sniffRunning_ = false;
        vmRunning_    = false;
        sgRunning_    = false;
        sgFreq_       = 1000U;
        sgDuty_       = 128U;  // 50 %
        vmHistIdx_    = 0U;
        lastSampleMs_ = 0U;
        lastXpMs_     = 0U;
        std::memset(vmHistory_, 0, sizeof(vmHistory_));
        g_sniffRing.reset();
        g_sniffActive  = false;
        g_sniffBytes   = 0U;
        g_sseNewData   = false;
        hexLinePos_    = 0U;
        std::memset(hexLine_, 0, sizeof(hexLine_));
        ESP_LOGI(TAG_HB, "HardwareBridge app started");
    }

    void onLoop() override
    {
        const uint32_t now = static_cast<uint32_t>(millis());

        switch (view_)
        {
        case HBView::SNIFFER:
            loopSniffer(now);
            break;
        case HBView::VOLTMETER:
            loopVoltmeter(now);
            break;
        case HBView::SIGGEN:
            // nothing to poll; PWM runs in HW
            break;
        default:
            break;
        }
    }

    void onDraw() override
    {
        auto &d = DisplayManager::instance();
        d.clear();

        switch (view_)
        {
        case HBView::MENU:     drawMenu(d);      break;
        case HBView::SNIFFER:  drawSniffer(d);   break;
        case HBView::VOLTMETER:drawVoltmeter(d); break;
        case HBView::SIGGEN:   drawSigGen(d);    break;
        }

        d.present();
    }

    void onEvent(Event *event) override
    {
        if (!event || event->type != EventType::EVT_INPUT) return;

        const auto input = static_cast<InputManager::InputEvent>(event->arg0);

        switch (view_)
        {
        case HBView::MENU:      handleMenuInput(input);    break;
        case HBView::SNIFFER:   handleSnifferInput(input); break;
        case HBView::VOLTMETER: handleVmInput(input);      break;
        case HBView::SIGGEN:    handleSgInput(input);      break;
        }
    }

    void onDestroy() override
    {
        stopSniffer();
        stopVoltmeter();
        stopSigGen();
        ESP_LOGI(TAG_HB, "HardwareBridge app destroyed");
    }

private:
    // ── State ────────────────────────────────────────────────────────────
    HBView   view_;
    uint8_t  menuSel_;

    // Sniffer
    bool     sniffRunning_;
    char     hexLine_[128];
    size_t   hexLinePos_;

    // Voltmeter
    bool     vmRunning_;
    uint16_t vmHistory_[VM_HISTORY_LEN];
    size_t   vmHistIdx_;
    uint32_t lastSampleMs_;
    float    currentVoltage_;

    // Signal generator
    bool     sgRunning_;
    uint32_t sgFreq_;
    uint8_t  sgDuty_;

    // XP
    uint32_t lastXpMs_;

    // ═════════════════════════════════════════════════════════════════════
    // MENU
    // ═════════════════════════════════════════════════════════════════════

    static constexpr size_t MENU_COUNT = 4U;
    static constexpr const char *MENU_LABELS[MENU_COUNT] = {
        "Bus Sniffer",
        "Voltmeter",
        "Signal Gen",
        "Back"
    };

    void drawMenu(DisplayManager &d)
    {
        d.drawText(0, 0, "HW Bridge");
        for (size_t i = 0U; i < MENU_COUNT; ++i)
        {
            const int16_t y = static_cast<int16_t>(HEADER_H + 2 + i * 12);
            if (i == menuSel_)
            {
                d.fillRect(0, y, DISPLAY_W, 11);
                d.drawText(4, y + 2, MENU_LABELS[i], 1U, 0);
            }
            else
            {
                d.drawText(4, y + 2, MENU_LABELS[i]);
            }
        }
    }

    void handleMenuInput(InputManager::InputEvent input)
    {
        switch (input)
        {
        case InputManager::InputEvent::UP:
            menuSel_ = (menuSel_ == 0U) ? static_cast<uint8_t>(MENU_COUNT - 1U)
                                         : static_cast<uint8_t>(menuSel_ - 1U);
            break;
        case InputManager::InputEvent::DOWN:
            menuSel_ = static_cast<uint8_t>((menuSel_ + 1U) % MENU_COUNT);
            break;
        case InputManager::InputEvent::CENTER:
        case InputManager::InputEvent::BUTTON_PRESS:
            selectMenuItem();
            break;
        case InputManager::InputEvent::LEFT:
        {
            const Event evt{EventType::EVT_SYSTEM, SYSTEM_EVENT_BACK, 0, nullptr};
            EventSystem::instance().postEvent(evt);
            break;
        }
        default:
            break;
        }
    }

    void selectMenuItem()
    {
        switch (menuSel_)
        {
        case 0U: view_ = HBView::SNIFFER;   startSniffer();   break;
        case 1U: view_ = HBView::VOLTMETER;  startVoltmeter(); break;
        case 2U: view_ = HBView::SIGGEN;     break;
        case 3U:
        {
            const Event evt{EventType::EVT_SYSTEM, SYSTEM_EVENT_BACK, 0, nullptr};
            EventSystem::instance().postEvent(evt);
            break;
        }
        default: break;
        }
    }

    // ═════════════════════════════════════════════════════════════════════
    // SNIFFER
    // ═════════════════════════════════════════════════════════════════════

    void startSniffer()
    {
        if (sniffRunning_) return;

        g_sniffRing.reset();
        g_sniffBytes  = 0U;
        g_sniffActive = true;
        hexLinePos_   = 0U;
        std::memset(hexLine_, 0, sizeof(hexLine_));

        switch (g_sniffProto)
        {
        case SniffProto::UART:
            g_sniffSerial.begin(115200, SERIAL_8N1, PIN_HB_UART_RX, PIN_HB_UART_TX);
            ESP_LOGI(TAG_HB, "UART sniffer started on RX=%u TX=%u",
                     PIN_HB_UART_RX, PIN_HB_UART_TX);
            break;

        case SniffProto::I2C:
            Wire.begin(PIN_HB_SDA, PIN_HB_SCL, 100000U);
            ESP_LOGI(TAG_HB, "I2C sniffer started on SDA=%u SCL=%u",
                     PIN_HB_SDA, PIN_HB_SCL);
            break;

        case SniffProto::SPI:
            pinMode(PIN_HB_SPI_CLK, INPUT);
            pinMode(PIN_HB_SPI_MOSI, INPUT);
            pinMode(PIN_HB_SPI_MISO, INPUT);
            ESP_LOGI(TAG_HB, "SPI sniffer started (CLK=%u MOSI=%u MISO=%u)",
                     PIN_HB_SPI_CLK, PIN_HB_SPI_MOSI, PIN_HB_SPI_MISO);
            break;

        default:
            break;
        }

        sniffRunning_ = true;
    }

    void stopSniffer()
    {
        if (!sniffRunning_) return;
        g_sniffActive = false;

        if (g_sniffProto == SniffProto::UART)
        {
            g_sniffSerial.end();
        }

        sniffRunning_ = false;
        ESP_LOGI(TAG_HB, "Sniffer stopped – %lu bytes captured",
                 static_cast<unsigned long>(g_sniffBytes));
    }

    void loopSniffer(uint32_t now)
    {
        if (!sniffRunning_) return;

        // Read from hardware into ring buffer
        switch (g_sniffProto)
        {
        case SniffProto::UART:
            while (g_sniffSerial.available() > 0)
            {
                const uint8_t b = static_cast<uint8_t>(g_sniffSerial.read());
                g_sniffRing.push(b);
                g_sniffBytes++;
            }
            break;

        case SniffProto::I2C:
        {
            // Passive I2C scan: request 1 byte from addresses 0x08-0x77
            // This is a simplified scan; real sniffer would use
            // GPIO interrupt-based edge detection.
            static uint8_t scanAddr = 0x08U;
            Wire.beginTransmission(scanAddr);
            uint8_t err = Wire.endTransmission(true);
            if (err == 0U)
            {
                // Device responded – request one byte
                Wire.requestFrom(scanAddr, static_cast<uint8_t>(1U));
                if (Wire.available())
                {
                    uint8_t b = static_cast<uint8_t>(Wire.read());
                    g_sniffRing.push(scanAddr);
                    g_sniffRing.push(b);
                    g_sniffBytes += 2U;
                }
            }
            scanAddr++;
            if (scanAddr > 0x77U) scanAddr = 0x08U;
            break;
        }

        case SniffProto::SPI:
        {
            // Bit-bang SPI sniff: sample MOSI on rising CLK edge
            static uint8_t spiByte  = 0U;
            static uint8_t spiBit   = 0U;
            static bool    lastClk  = false;

            bool clk = digitalRead(PIN_HB_SPI_CLK);
            if (clk && !lastClk) // rising edge
            {
                bool mosi = digitalRead(PIN_HB_SPI_MOSI);
                spiByte = static_cast<uint8_t>((spiByte << 1U) | (mosi ? 1U : 0U));
                spiBit++;
                if (spiBit >= 8U)
                {
                    g_sniffRing.push(spiByte);
                    g_sniffBytes++;
                    spiByte = 0U;
                    spiBit  = 0U;
                }
            }
            lastClk = clk;
            break;
        }

        default:
            break;
        }

        // Build hex-dump line for display + SSE passthrough
        uint8_t b;
        while (g_sniffRing.pop(b))
        {
            char hex[4];
            std::snprintf(hex, sizeof(hex), "%02X ", b);
            for (size_t i = 0U; hex[i] != '\0' && hexLinePos_ < sizeof(hexLine_) - 1U; ++i)
            {
                hexLine_[hexLinePos_++] = hex[i];
            }

            // When line is full, push to SSE buffer and reset
            if (hexLinePos_ >= 48U)  // 16 bytes × 3 chars each
            {
                hexLine_[hexLinePos_] = '\0';
                std::strncpy(g_sseLineBuffer, hexLine_, SSE_LINE_MAX - 1U);
                g_sseLineBuffer[SSE_LINE_MAX - 1U] = '\0';
                g_sseNewData = true;
                hexLinePos_ = 0U;
            }
        }

        // Award XP for captured data
        if (g_sniffBytes > 0U && (now - lastXpMs_) >= XP_COOLDOWN_MS)
        {
            ExperienceManager::instance().addXP(10);
            lastXpMs_ = now;
        }
    }

    void drawSniffer(DisplayManager &d)
    {
        char hdr[32];
        std::snprintf(hdr, sizeof(hdr), "Sniff:%s %luB",
                      sniffProtoName(g_sniffProto),
                      static_cast<unsigned long>(g_sniffBytes));
        d.drawText(0, 0, hdr);

        // Show current hex line
        if (hexLinePos_ > 0U)
        {
            hexLine_[hexLinePos_] = '\0';
            // split into 2 lines of 21 chars each for 128px width
            char line1[22] = {};
            char line2[22] = {};
            std::strncpy(line1, hexLine_,
                         (hexLinePos_ > 21U) ? 21U : hexLinePos_);
            if (hexLinePos_ > 21U)
            {
                std::strncpy(line2, hexLine_ + 21U,
                             (hexLinePos_ - 21U > 21U) ? 21U : hexLinePos_ - 21U);
            }
            d.drawText(0, 14, line1);
            d.drawText(0, 24, line2);
        }
        else
        {
            d.drawText(0, 14, "Waiting for data...");
        }

        // Protocol selector hint
        d.drawText(0, 38, "U/D:protocol L:back");

        // Ring buffer usage bar
        const size_t used = g_sniffRing.available();
        const int16_t barW = static_cast<int16_t>((used * 120U) / SNIFF_BUF_SIZE);
        d.drawRect(0, 54, 124, 8);
        if (barW > 0)
        {
            d.fillRect(2, 56, barW, 4);
        }

        // Show passthrough status
        d.drawText(0, 48, sniffRunning_ ? "[LIVE->Dashboard]" : "[STOPPED]");
    }

    void handleSnifferInput(InputManager::InputEvent input)
    {
        switch (input)
        {
        case InputManager::InputEvent::UP:
        {
            // Cycle protocol forward
            uint8_t p = static_cast<uint8_t>(g_sniffProto);
            p = (p + 1U) % static_cast<uint8_t>(SniffProto::COUNT);
            g_sniffProto = static_cast<SniffProto>(p);
            stopSniffer();
            startSniffer();
            break;
        }
        case InputManager::InputEvent::DOWN:
        {
            uint8_t p = static_cast<uint8_t>(g_sniffProto);
            p = (p == 0U) ? static_cast<uint8_t>(SniffProto::COUNT) - 1U
                          : static_cast<uint8_t>(p - 1U);
            g_sniffProto = static_cast<SniffProto>(p);
            stopSniffer();
            startSniffer();
            break;
        }
        case InputManager::InputEvent::LEFT:
            stopSniffer();
            view_ = HBView::MENU;
            break;
        default:
            break;
        }
    }

    // ═════════════════════════════════════════════════════════════════════
    // VOLTMETER
    // ═════════════════════════════════════════════════════════════════════

    void startVoltmeter()
    {
        vmRunning_ = true;
        vmHistIdx_ = 0U;
        currentVoltage_ = 0.0f;
        std::memset(vmHistory_, 0, sizeof(vmHistory_));
        analogReadResolution(12);
        pinMode(PIN_HB_ADC, INPUT);
        ESP_LOGI(TAG_HB, "Voltmeter started on ADC pin %u", PIN_HB_ADC);
    }

    void stopVoltmeter()
    {
        vmRunning_ = false;
    }

    void loopVoltmeter(uint32_t now)
    {
        if (!vmRunning_) return;
        if ((now - lastSampleMs_) < VM_SAMPLE_MS) return;
        lastSampleMs_ = now;

        const uint16_t raw = static_cast<uint16_t>(analogRead(PIN_HB_ADC));
        currentVoltage_ = (static_cast<float>(raw) / static_cast<float>(ADC_MAX)) * ADC_REF_VOLTAGE;
        g_lastVoltage = currentVoltage_;

        vmHistory_[vmHistIdx_ % VM_HISTORY_LEN] = raw;
        vmHistIdx_++;
    }

    void drawVoltmeter(DisplayManager &d)
    {
        // Header with voltage reading
        char hdr[24];
        std::snprintf(hdr, sizeof(hdr), "VM: %.2fV", static_cast<double>(currentVoltage_));
        d.drawText(0, 0, hdr);

        // Large voltage display
        char bigV[10];
        std::snprintf(bigV, sizeof(bigV), "%.3f", static_cast<double>(currentVoltage_));
        d.drawText(30, 12, bigV);
        d.drawText(80, 12, "V");

        // Bar graph: 0V..3.3V mapped to 0..120 pixels
        const int16_t barMaxW = 120;
        const int16_t barH    = 10;
        const int16_t barY    = 26;
        const int16_t barVal  = static_cast<int16_t>(
            (currentVoltage_ / ADC_REF_VOLTAGE) * static_cast<float>(barMaxW));

        d.drawRect(2, barY, barMaxW + 2, barH);
        if (barVal > 0)
        {
            d.fillRect(3, barY + 1, barVal, barH - 2);
        }

        // Scale labels
        d.drawText(0, barY + barH + 2, "0");
        d.drawText(55, barY + barH + 2, "1.65");
        d.drawText(110, barY + barH + 2, "3.3");

        // Scrolling history waveform (bottom area)
        const int16_t waveY = 48;
        const int16_t waveH = 14;
        const size_t  count = (vmHistIdx_ < VM_HISTORY_LEN) ? vmHistIdx_ : VM_HISTORY_LEN;
        const size_t  start = (vmHistIdx_ >= VM_HISTORY_LEN) ? (vmHistIdx_ - VM_HISTORY_LEN) : 0U;

        for (size_t i = 0U; i < count && i < static_cast<size_t>(DISPLAY_W); ++i)
        {
            const uint16_t raw = vmHistory_[(start + i) % VM_HISTORY_LEN];
            const int16_t h = static_cast<int16_t>(
                (static_cast<float>(raw) / static_cast<float>(ADC_MAX)) * static_cast<float>(waveH));
            const int16_t x = static_cast<int16_t>(i);
            d.drawLine(x, waveY + waveH - h, x, waveY + waveH);
        }
    }

    void handleVmInput(InputManager::InputEvent input)
    {
        if (input == InputManager::InputEvent::LEFT)
        {
            stopVoltmeter();
            view_ = HBView::MENU;
        }
    }

    // ═════════════════════════════════════════════════════════════════════
    // SIGNAL GENERATOR
    // ═════════════════════════════════════════════════════════════════════

    void startSigGen()
    {
        if (sgRunning_) return;

        ledc_timer_config_t timerCfg = {};
        timerCfg.speed_mode      = LEDC_LOW_SPEED_MODE;
        timerCfg.timer_num       = SIGGEN_LEDC_TIMER;
        timerCfg.duty_resolution = static_cast<ledc_timer_bit_t>(SIGGEN_DUTY_BITS);
        timerCfg.freq_hz         = sgFreq_;
        timerCfg.clk_cfg         = LEDC_AUTO_CLK;
        ledc_timer_config(&timerCfg);

        ledc_channel_config_t chCfg = {};
        chCfg.speed_mode = LEDC_LOW_SPEED_MODE;
        chCfg.channel    = SIGGEN_LEDC_CH;
        chCfg.timer_sel  = SIGGEN_LEDC_TIMER;
        chCfg.intr_type  = LEDC_INTR_DISABLE;
        chCfg.gpio_num   = PIN_HB_SIGGEN;
        chCfg.duty       = sgDuty_;
        chCfg.hpoint     = 0;
        ledc_channel_config(&chCfg);

        sgRunning_ = true;
        ESP_LOGI(TAG_HB, "SigGen started: %lu Hz, duty=%u/255",
                 static_cast<unsigned long>(sgFreq_), sgDuty_);
    }

    void stopSigGen()
    {
        if (!sgRunning_) return;
        ledc_stop(LEDC_LOW_SPEED_MODE, SIGGEN_LEDC_CH, 0);
        sgRunning_ = false;
        ESP_LOGI(TAG_HB, "SigGen stopped");
    }

    void updateSigGen()
    {
        if (!sgRunning_) return;

        ledc_set_freq(LEDC_LOW_SPEED_MODE, SIGGEN_LEDC_TIMER, sgFreq_);
        ledc_set_duty(LEDC_LOW_SPEED_MODE, SIGGEN_LEDC_CH, sgDuty_);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, SIGGEN_LEDC_CH);
    }

    void drawSigGen(DisplayManager &d)
    {
        d.drawText(0, 0, "Signal Generator");

        // Frequency display
        char freq[24];
        if (sgFreq_ >= 1000U)
        {
            std::snprintf(freq, sizeof(freq), "F: %lu.%01lu kHz",
                          static_cast<unsigned long>(sgFreq_ / 1000U),
                          static_cast<unsigned long>((sgFreq_ % 1000U) / 100U));
        }
        else
        {
            std::snprintf(freq, sizeof(freq), "F: %lu Hz",
                          static_cast<unsigned long>(sgFreq_));
        }
        d.drawText(0, 14, freq);

        // Duty cycle
        const uint8_t dutyPct = static_cast<uint8_t>((static_cast<uint16_t>(sgDuty_) * 100U) / 255U);
        char duty[20];
        std::snprintf(duty, sizeof(duty), "D: %u%%", dutyPct);
        d.drawText(0, 26, duty);

        // Status
        d.drawText(0, 38, sgRunning_ ? "[RUNNING]" : "[STOPPED]");

        // Visual PWM waveform preview
        const int16_t waveY = 48;
        const int16_t waveH = 12;
        const int16_t onW   = static_cast<int16_t>((static_cast<uint16_t>(sgDuty_) * 30U) / 255U);
        const int16_t offW  = static_cast<int16_t>(30 - onW);

        for (int16_t cycle = 0; cycle < 4; ++cycle)
        {
            const int16_t cx = cycle * 32;
            // HIGH portion
            if (onW > 0)
            {
                d.drawLine(cx, waveY, cx, waveY + waveH);            // rising edge
                d.drawLine(cx, waveY, cx + onW, waveY);              // high
                d.drawLine(cx + onW, waveY, cx + onW, waveY + waveH); // falling edge
            }
            // LOW portion
            if (offW > 0)
            {
                d.drawLine(cx + onW, waveY + waveH,
                            cx + onW + offW, waveY + waveH);         // low
            }
        }

        d.drawText(0, DISPLAY_H - 8, "U/D:freq C:run L:bk");
    }

    void handleSgInput(InputManager::InputEvent input)
    {
        switch (input)
        {
        case InputManager::InputEvent::UP:
            // Increase frequency (logarithmic steps)
            if (sgFreq_ < 100U)         sgFreq_ += 10U;
            else if (sgFreq_ < 1000U)   sgFreq_ += 100U;
            else if (sgFreq_ < 10000U)  sgFreq_ += 1000U;
            else if (sgFreq_ < 100000U) sgFreq_ += 10000U;
            else                         sgFreq_ += 100000U;
            if (sgFreq_ > SIGGEN_FREQ_MAX) sgFreq_ = SIGGEN_FREQ_MAX;
            updateSigGen();
            break;

        case InputManager::InputEvent::DOWN:
            if (sgFreq_ > 100000U)      sgFreq_ -= 100000U;
            else if (sgFreq_ > 10000U)  sgFreq_ -= 10000U;
            else if (sgFreq_ > 1000U)   sgFreq_ -= 1000U;
            else if (sgFreq_ > 100U)    sgFreq_ -= 100U;
            else if (sgFreq_ > 10U)     sgFreq_ -= 10U;
            if (sgFreq_ < SIGGEN_FREQ_MIN) sgFreq_ = SIGGEN_FREQ_MIN;
            updateSigGen();
            break;

        case InputManager::InputEvent::RIGHT:
            // Cycle duty: 25% → 50% → 75% → 25%
            if (sgDuty_ < 96U)        sgDuty_ = 128U;
            else if (sgDuty_ < 160U)  sgDuty_ = 192U;
            else                       sgDuty_ = 64U;
            updateSigGen();
            break;

        case InputManager::InputEvent::CENTER:
        case InputManager::InputEvent::BUTTON_PRESS:
            if (sgRunning_)
                stopSigGen();
            else
                startSigGen();
            break;

        case InputManager::InputEvent::LEFT:
            stopSigGen();
            view_ = HBView::MENU;
            break;

        default:
            break;
        }
    }

public:
    // ═════════════════════════════════════════════════════════════════════
    // SSE Passthrough – called from the Dashboard HTTP handler
    // ═════════════════════════════════════════════════════════════════════

    /// Dashboard SSE handler for /api/hwbridge/live
    static esp_err_t sseHandler(httpd_req_t *req)
    {
        httpd_resp_set_type(req, "text/event-stream");
        httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
        httpd_resp_set_hdr(req, "Connection", "keep-alive");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

        for (int i = 0; i < SSE_MAX_SAMPLES; ++i)
        {
            char buf[SSE_LINE_MAX + 64];

            if (g_sseNewData)
            {
                int len = snprintf(buf, sizeof(buf),
                    "data: {\"type\":\"sniff\",\"proto\":\"%s\",\"hex\":\"%s\",\"total\":%lu}\n\n",
                    sniffProtoName(g_sniffProto),
                    g_sseLineBuffer,
                    static_cast<unsigned long>(g_sniffBytes));
                g_sseNewData = false;

                if (httpd_resp_send_chunk(req, buf, len) != ESP_OK)
                    break;
            }
            else
            {
                // Send voltage reading if voltmeter is active
                int len = snprintf(buf, sizeof(buf),
                    "data: {\"type\":\"vm\",\"voltage\":%.3f}\n\n",
                    static_cast<double>(g_lastVoltage));

                if (httpd_resp_send_chunk(req, buf, len) != ESP_OK)
                    break;
            }

            vTaskDelay(pdMS_TO_TICKS(SSE_INTERVAL_MS));
        }

        return httpd_resp_send_chunk(req, nullptr, 0);
    }
};

// Constexpr static member definitions
constexpr const char *HardwareBridgeApp::MENU_LABELS[MENU_COUNT];

} // anonymous namespace

// ── Factory function ─────────────────────────────────────────────────────────

AppBase *createHardwareBridgeApp()
{
    return new (std::nothrow) HardwareBridgeApp();
}
