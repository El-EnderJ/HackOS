/**
 * @file module_detect.h
 * @brief Smart Module Auto-Discovery – Plug & Play hardware detection.
 *
 * Probes I2C and SPI buses at boot to detect external hardware modules:
 *  - **NRF24L01+**: 2.4 GHz transceiver for wireless mouse/keyboard injection.
 *  - **CC1101**: Sub-GHz transceiver for extended-range RF attacks.
 *
 * When a module is detected, the corresponding app is automatically
 * unlocked in the launcher and a toast notification is shown.
 *
 * Supported detection methods:
 *  - NRF24: SPI read of STATUS register (0x07) – valid if bits [6:0] != 0xFF.
 *  - CC1101: SPI read of PARTNUM (0xF0) and VERSION (0xF1) registers.
 *
 * Default SPI pin assignments (shared VSPI bus):
 *  - NRF24 CS: GPIO 2 (configurable)
 *  - CC1101 CS: GPIO 15 (configurable)
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace hackos::core {

/// @brief Maximum number of detectable external modules.
static constexpr size_t MAX_MODULES = 4U;

/// @brief Module type identifier.
enum class ModuleType : uint8_t
{
    NONE = 0,
    NRF24,   ///< NRF24L01+ 2.4 GHz transceiver
    CC1101,  ///< CC1101 Sub-GHz transceiver
};

/// @brief Returns a human-readable name for a module type.
const char *moduleTypeName(ModuleType type);

/// @brief Detected module information.
struct ModuleInfo
{
    ModuleType type;
    uint8_t csPin;       ///< Chip-select pin used
    uint8_t version;     ///< Hardware version/ID read from module
    bool detected;
};

// ── ModuleDetect ─────────────────────────────────────────────────────────────

class ModuleDetect
{
public:
    static ModuleDetect &instance();

    /**
     * @brief Probe all known module CS pins and detect connected hardware.
     *
     * Should be called once during boot after SPI is initialized.
     * Sends toast notifications for each detected module.
     *
     * @return Number of modules detected.
     */
    size_t probeAll();

    /// @brief Number of detected modules.
    size_t detectedCount() const;

    /// @brief Get info for a detected module by index.
    const ModuleInfo *moduleAt(size_t index) const;

    /// @brief Check if a specific module type was detected.
    bool isDetected(ModuleType type) const;

private:
    ModuleDetect();

    /// @brief Probe NRF24L01+ via SPI STATUS register read.
    bool probeNRF24(uint8_t csPin, uint8_t &version);

    /// @brief Probe CC1101 via SPI PARTNUM/VERSION register read.
    bool probeCC1101(uint8_t csPin, uint8_t &version);

    /// @brief SPI single-byte register read helper.
    uint8_t spiReadReg(uint8_t csPin, uint8_t reg);

    ModuleInfo modules_[MAX_MODULES];
    size_t moduleCount_;
};

} // namespace hackos::core
