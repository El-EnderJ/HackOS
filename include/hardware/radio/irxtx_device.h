/**
 * @file irxtx_device.h
 * @brief Generic hardware abstraction interface for radio RX/TX devices.
 *
 * Every physical transceiver (IR LED, 433 MHz OOK module, PN532 NFC reader)
 * implements this interface so that the RadioManager can operate on any
 * device without knowing its concrete type.
 *
 * Library-specific details (IRremoteESP8266, RCSwitch, Adafruit_PN532) are
 * hidden behind the concrete implementations.
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace hackos::radio {

// ── Modulation type tag ──────────────────────────────────────────────────────

/// @brief Modulation schemes understood by the radio subsystem.
enum class Modulation : uint8_t
{
    OOK,      ///< On-Off Keying (433 MHz, garage doors, weather stations)
    ASK,      ///< Amplitude-Shift Keying (generic)
    PSK,      ///< Phase-Shift Keying (reserved)
    NFC_A,    ///< ISO 14443-A (Mifare, NTAG)
    IR_PULSE, ///< Modulated IR carrier (NEC, RC5, …)
};

// ── IRxTxDevice ──────────────────────────────────────────────────────────────

/**
 * @brief Abstract interface for any radio receive/transmit device.
 *
 * Concrete adapters wrap the low-level library calls behind this API so
 * that the RadioManager and protocol parsers can be tested and extended
 * independently of the physical hardware.
 */
class IRxTxDevice
{
public:
    virtual ~IRxTxDevice() = default;

    // ── Lifecycle ────────────────────────────────────────────────────────

    /// @brief Initialise the device for receiving.
    /// @return true on success.
    virtual bool startReceive() = 0;

    /// @brief Initialise the device for transmitting.
    /// @return true on success.
    virtual bool startTransmit() = 0;

    /// @brief Release all hardware resources.
    virtual void stop() = 0;

    /// @brief Query whether the device is currently receiving.
    virtual bool isReceiving() const = 0;

    /// @brief Query whether the device is currently transmitting.
    virtual bool isTransmitting() const = 0;

    // ── Data exchange ────────────────────────────────────────────────────

    /**
     * @brief Read raw sample data from the device into @p buffer.
     *
     * @param buffer  Destination buffer.
     * @param maxLen  Maximum bytes to read.
     * @return Number of bytes actually placed into @p buffer (0 = nothing available).
     */
    virtual size_t read(uint8_t *buffer, size_t maxLen) = 0;

    /**
     * @brief Write raw sample data to the device for transmission.
     *
     * @param data  Pointer to the payload bytes.
     * @param len   Length in bytes.
     * @return true if the device accepted the data.
     */
    virtual bool write(const uint8_t *data, size_t len) = 0;

    // ── Metadata ─────────────────────────────────────────────────────────

    /// @brief Human-readable name of the device (e.g. "IR_TX", "OOK_433").
    virtual const char *name() const = 0;

    /// @brief The modulation type this device operates on.
    virtual Modulation modulation() const = 0;
};

} // namespace hackos::radio
