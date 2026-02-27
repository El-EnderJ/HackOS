/**
 * @file protocol_nec.h
 * @brief IR NEC protocol parser for the HackOS radio subsystem.
 *
 * The NEC protocol is one of the most common IR remote control protocols.
 * It uses pulse-distance encoding:
 *
 *  - Leader:  9 ms mark + 4.5 ms space
 *  - Bit '0': 562.5 µs mark + 562.5 µs space
 *  - Bit '1': 562.5 µs mark + 1687.5 µs space
 *  - Standard frame: 8-bit address + 8-bit inverted address +
 *                    8-bit command + 8-bit inverted command = 32 bits
 *
 * This parser supplements the IRremoteESP8266 library-level decoding by
 * providing a unified RadioProtocol interface so that NEC signals can
 * be stored in the standard SignalRecord format alongside OOK and NFC
 * captures.
 */

#pragma once

#include "radio_protocol.h"

namespace hackos::radio {

/**
 * @brief Protocol parser for NEC-encoded infrared signals.
 */
class Protocol_NEC : public RadioProtocol
{
public:
    Protocol_NEC() = default;

    const char *name() const override { return "NEC"; }
    Modulation modulation() const override { return Modulation::IR_PULSE; }

    /**
     * @brief Attempt NEC decoding on raw IR timing data.
     *
     * @param rawTimings  Pulse/gap array (µs, positive = mark).
     * @param count       Number of entries.
     * @param[out] record Filled with NEC metadata on success.
     * @return true if the signal matches the NEC protocol.
     */
    bool tryDecode(const int32_t *rawTimings,
                   size_t count,
                   SignalRecord &record) override;

    /**
     * @brief Encode a 32-bit NEC frame into raw timings.
     *
     * @param record       Source signal record (bitCount must be 32).
     * @param[out] timings Destination array.
     * @param maxTimings   Destination capacity.
     * @return Number of timing entries written.
     */
    size_t encode(const SignalRecord &record,
                  int32_t *timings,
                  size_t maxTimings) override;

private:
    /// Default IR carrier frequency (Hz).
    static constexpr uint32_t CARRIER_HZ = 38000U;

    /// NEC leader mark duration (µs).
    static constexpr int32_t LEADER_MARK_US = 9000;

    /// NEC leader space duration (µs).
    static constexpr int32_t LEADER_SPACE_US = 4500;

    /// NEC bit mark duration (µs).
    static constexpr int32_t BIT_MARK_US = 562;

    /// NEC logical '0' space duration (µs).
    static constexpr int32_t ZERO_SPACE_US = 562;

    /// NEC logical '1' space duration (µs).
    static constexpr int32_t ONE_SPACE_US = 1687;

    /// Number of data bits in a standard NEC frame.
    static constexpr uint16_t NEC_BITS = 32U;

    /// Timing tolerance (30 %).
    static constexpr int32_t TOLERANCE_PCT = 30;

    /// @brief Check whether @p measured is within tolerance of @p nominal.
    static bool matchesDuration(int32_t measured, int32_t nominal);
};

} // namespace hackos::radio
