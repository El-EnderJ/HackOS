/**
 * @file module_detect.cpp
 * @brief Smart Module Auto-Discovery implementation.
 *
 * Uses SPI register reads to identify NRF24L01+ and CC1101 modules.
 * Each probe is non-destructive: only reads identification registers
 * without altering module state.
 */

#include "core/module_detect.h"

#include <cstring>

#include <Arduino.h>
#include <SPI.h>
#include <esp_log.h>

#include "config.h"
#include "ui/toast_manager.h"

static constexpr const char *TAG_MOD = "ModuleDetect";

namespace hackos::core {

// ── Default CS pin assignments ───────────────────────────────────────────────
// These use available GPIO pins not already in use by other peripherals.

/// @note GPIO 2 is the onboard LED on many ESP32 DevKit boards.
///       If the LED conflicts, change to an available GPIO (e.g., 12, 13).
static constexpr uint8_t PIN_NRF24_CS  = 2U;   ///< NRF24 chip-select
static constexpr uint8_t PIN_CC1101_CS = 15U;   ///< CC1101 chip-select

// ── NRF24L01+ register addresses ─────────────────────────────────────────────

static constexpr uint8_t NRF24_REG_STATUS  = 0x07U;  ///< STATUS register
static constexpr uint8_t NRF24_REG_CONFIG  = 0x00U;  ///< CONFIG register
static constexpr uint8_t NRF24_CMD_R_REG   = 0x00U;  ///< Read register command

// ── CC1101 register addresses ────────────────────────────────────────────────

static constexpr uint8_t CC1101_REG_PARTNUM = 0x30U | 0xC0U; ///< PARTNUM (burst read)
static constexpr uint8_t CC1101_REG_VERSION = 0x31U | 0xC0U; ///< VERSION (burst read)

// ── Module name lookup ───────────────────────────────────────────────────────

const char *moduleTypeName(ModuleType type)
{
    switch (type)
    {
    case ModuleType::NRF24:  return "NRF24L01+";
    case ModuleType::CC1101: return "CC1101";
    case ModuleType::NONE:   return "None";
    }
    return "Unknown";
}

// ── Singleton ────────────────────────────────────────────────────────────────

ModuleDetect &ModuleDetect::instance()
{
    static ModuleDetect inst;
    return inst;
}

ModuleDetect::ModuleDetect()
    : moduleCount_(0U)
{
    std::memset(modules_, 0, sizeof(modules_));
}

// ── Public API ───────────────────────────────────────────────────────────────

size_t ModuleDetect::probeAll()
{
    moduleCount_ = 0U;
    std::memset(modules_, 0, sizeof(modules_));

    ESP_LOGI(TAG_MOD, "Probing for external modules...");

    // Probe NRF24L01+
    {
        uint8_t version = 0U;
        if (probeNRF24(PIN_NRF24_CS, version))
        {
            modules_[moduleCount_].type = ModuleType::NRF24;
            modules_[moduleCount_].csPin = PIN_NRF24_CS;
            modules_[moduleCount_].version = version;
            modules_[moduleCount_].detected = true;
            ++moduleCount_;

            ESP_LOGI(TAG_MOD, "NRF24L01+ detected (CS=%u, ver=0x%02X)",
                     PIN_NRF24_CS, version);
            ToastManager::instance().show("[+] NRF24L01+ detected!");
        }
        else
        {
            ESP_LOGI(TAG_MOD, "NRF24L01+ not found on CS=%u", PIN_NRF24_CS);
        }
    }

    // Probe CC1101
    {
        uint8_t version = 0U;
        if (probeCC1101(PIN_CC1101_CS, version))
        {
            modules_[moduleCount_].type = ModuleType::CC1101;
            modules_[moduleCount_].csPin = PIN_CC1101_CS;
            modules_[moduleCount_].version = version;
            modules_[moduleCount_].detected = true;
            ++moduleCount_;

            ESP_LOGI(TAG_MOD, "CC1101 detected (CS=%u, ver=0x%02X)",
                     PIN_CC1101_CS, version);
            ToastManager::instance().show("[+] CC1101 detected!");
        }
        else
        {
            ESP_LOGI(TAG_MOD, "CC1101 not found on CS=%u", PIN_CC1101_CS);
        }
    }

    ESP_LOGI(TAG_MOD, "Module probe complete: %u module(s) detected",
             static_cast<unsigned>(moduleCount_));
    return moduleCount_;
}

size_t ModuleDetect::detectedCount() const
{
    return moduleCount_;
}

const ModuleInfo *ModuleDetect::moduleAt(size_t index) const
{
    if (index >= moduleCount_)
    {
        return nullptr;
    }
    return &modules_[index];
}

bool ModuleDetect::isDetected(ModuleType type) const
{
    for (size_t i = 0U; i < moduleCount_; ++i)
    {
        if (modules_[i].type == type && modules_[i].detected)
        {
            return true;
        }
    }
    return false;
}

// ── NRF24L01+ probe ─────────────────────────────────────────────────────────

bool ModuleDetect::probeNRF24(uint8_t csPin, uint8_t &version)
{
    pinMode(csPin, OUTPUT);
    digitalWrite(csPin, HIGH);

    // Read STATUS register – should return a value with recognizable bits
    uint8_t status = spiReadReg(csPin, NRF24_CMD_R_REG | NRF24_REG_STATUS);

    // NRF24 STATUS register has bits [6:4] = RX_P_NO (pipe number),
    // bit 0 = TX_FULL. If all bits are 1 (0xFF) or 0, no module present.
    if (status == 0xFFU || status == 0x00U)
    {
        return false;
    }

    // Double-check: read CONFIG register
    uint8_t config = spiReadReg(csPin, NRF24_CMD_R_REG | NRF24_REG_CONFIG);

    // CONFIG reset value is 0x08 (EN_CRC set). Bits 7 should be 0.
    if (config == 0xFFU || (config & 0x80U) != 0U)
    {
        return false;
    }

    version = status;
    return true;
}

// ── CC1101 probe ─────────────────────────────────────────────────────────────

bool ModuleDetect::probeCC1101(uint8_t csPin, uint8_t &version)
{
    pinMode(csPin, OUTPUT);
    digitalWrite(csPin, HIGH);

    // Read PARTNUM register – should be 0x00 for CC1101
    uint8_t partnum = spiReadReg(csPin, CC1101_REG_PARTNUM);

    if (partnum != 0x00U)
    {
        return false;
    }

    // Read VERSION register – should be 0x14 for CC1101
    uint8_t ver = spiReadReg(csPin, CC1101_REG_VERSION);

    if (ver == 0x00U || ver == 0xFFU)
    {
        return false;
    }

    version = ver;
    return true;
}

// ── SPI helper ───────────────────────────────────────────────────────────────

uint8_t ModuleDetect::spiReadReg(uint8_t csPin, uint8_t reg)
{
    SPI.beginTransaction(SPISettings(1000000UL, MSBFIRST, SPI_MODE0));
    digitalWrite(csPin, LOW);
    (void)SPI.transfer(reg);           // Send register address
    uint8_t val = SPI.transfer(0x00U); // Read response
    digitalWrite(csPin, HIGH);
    SPI.endTransaction();
    return val;
}

} // namespace hackos::core
