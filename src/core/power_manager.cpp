/**
 * @file power_manager.cpp
 * @brief PowerManager implementation – CPU frequency scaling + Light Sleep.
 */

#include "core/power_manager.h"

#include <Arduino.h>
#include <esp_log.h>
#include <esp_pm.h>
#include <esp_sleep.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>

static constexpr const char *TAG = "PowerMgr";

namespace hackos::core {

// ── Singleton ────────────────────────────────────────────────────────────────

PowerManager &PowerManager::instance()
{
    static PowerManager pm;
    return pm;
}

PowerManager::PowerManager()
    : wakeGpio_(0U),
      lastActivityMs_(0U),
      lowPowerActive_(false),
      initialized_(false)
{
}

// ── Initialisation ───────────────────────────────────────────────────────────

bool PowerManager::init(uint8_t wakeGpio)
{
    if (initialized_)
    {
        return true;
    }

    wakeGpio_ = wakeGpio;
    lastActivityMs_ = millis();

    // Configure the joystick button GPIO as a Light-Sleep wake source.
    const gpio_num_t gpioNum = static_cast<gpio_num_t>(wakeGpio_);

    // Enable the GPIO as a light-sleep wake-up source (low level = pressed).
    const esp_err_t err = gpio_wakeup_enable(gpioNum, GPIO_INTR_LOW_LEVEL);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "gpio_wakeup_enable failed: %s", esp_err_to_name(err));
        return false;
    }

    esp_sleep_enable_gpio_wakeup();

    // Start in high-performance mode.
    setCpuFrequencyMhz(240);
    lowPowerActive_ = false;
    initialized_ = true;

    ESP_LOGI(TAG, "Initialised (wake GPIO=%u, CPU=240 MHz)", wakeGpio_);
    return true;
}

// ── Frequency control ────────────────────────────────────────────────────────

void PowerManager::setHighPerformance()
{
    if (lowPowerActive_)
    {
        setCpuFrequencyMhz(240);
        lowPowerActive_ = false;
        ESP_LOGI(TAG, "CPU → 240 MHz");
    }
}

void PowerManager::setLowPower()
{
    if (!lowPowerActive_)
    {
        setCpuFrequencyMhz(80);
        lowPowerActive_ = true;
        ESP_LOGI(TAG, "CPU → 80 MHz");
    }
}

uint32_t PowerManager::currentFrequencyMHz() const
{
    return static_cast<uint32_t>(getCpuFrequencyMhz());
}

// ── Activity tracking ────────────────────────────────────────────────────────

void PowerManager::onActivity()
{
    lastActivityMs_ = millis();

    if (lowPowerActive_)
    {
        setHighPerformance();
    }
}

void PowerManager::tick(uint32_t idleThresholdMs, uint32_t sleepThresholdMs)
{
    if (!initialized_)
    {
        return;
    }

    const uint32_t now = millis();
    const uint32_t elapsed = now - lastActivityMs_;

    if (elapsed >= sleepThresholdMs)
    {
        ESP_LOGI(TAG, "Idle %lu ms – entering Light Sleep", static_cast<unsigned long>(elapsed));
        enterLightSleep();
        // After wake-up, reset activity timer and restore performance.
        onActivity();
        return;
    }

    if (elapsed >= idleThresholdMs && !lowPowerActive_)
    {
        setLowPower();
    }
}

void PowerManager::enterLightSleep()
{
    ESP_LOGI(TAG, "Entering Light Sleep (wake on GPIO %u)", wakeGpio_);

    // Ensure the CPU is at low freq before sleeping to reduce transition current.
    setLowPower();

    esp_light_sleep_start();

    // Execution resumes here after wake-up.
    ESP_LOGI(TAG, "Woke from Light Sleep");
}

} // namespace hackos::core
