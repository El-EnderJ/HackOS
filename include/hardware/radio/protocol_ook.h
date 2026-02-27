/**
 * @file protocol_ook.h
 * @brief 433 MHz OOK (On-Off Keying) protocol parser.
 *
 * Handles the decoding and encoding of common OOK-modulated signals
 * found in garage door openers, wireless doorbells, weather stations,
 * and similar Sub-GHz devices operating around 433.92 MHz.
 *
 * Timing thresholds are based on the widespread EV1527 / PT2262 encoder
 * chips which use a short-pulse / long-pulse scheme with a sync preamble.
 *
 * ## Registering a new 433 MHz garage door protocol
 *
 * 1. Capture the raw signal using RadioManager::startCapture().
 *    The hardware interrupt fills the RingBuffer with pulse/gap timings.
 *
 * 2. The worker task drains the buffer and calls
 *    `ProtocolRegistry::tryDecodeAll()`.  If Protocol_OOK matches, a
 *    `SignalRecord` is created with:
 *      - `frequencyHz  = 433920000`
 *      - `modulation   = Modulation::OOK`
 *      - `protocolName = "OOK"`
 *      - `bitCount`    = detected data bits
 *      - `decodedValue`= numeric code
 *      - `rawTimings[]`= the original pulse/gap array (µs)
 *
 * 3. The record can be saved to SD card via the StorageManager, or
 *    replayed through the RFTransceiver using `Protocol_OOK::encode()`.
 *
 * 4. To add a *new* OOK sub-variant (e.g. a proprietary rolling-code
 *    garage remote), subclass `Protocol_OOK`, override `tryDecode()` to
 *    recognise the new sync/bit pattern, and register the instance with:
 *    @code
 *        static MyGarageDoorProtocol myProto;
 *        ProtocolRegistry::registerProtocol(&myProto);
 *    @endcode
 */

#pragma once

#include "radio_protocol.h"

namespace hackos::radio {

/**
 * @brief Protocol parser for generic OOK / ASK 433 MHz signals.
 *
 * Decoding strategy:
 *  - A sync preamble is detected when a gap ≥ SYNC_MIN_US is found.
 *  - Data bits follow: a short-pulse + long-gap encodes a '0',
 *    and a long-pulse + short-gap encodes a '1' (EV1527 convention).
 *  - The parser is tolerant of ±25 % timing variation.
 */
class Protocol_OOK : public RadioProtocol
{
public:
    Protocol_OOK() = default;

    const char *name() const override { return "OOK"; }
    Modulation modulation() const override { return Modulation::OOK; }

    /**
     * @brief Attempt OOK decoding on raw timing data.
     *
     * @param rawTimings  Pulse/gap array (positive = mark, negative = space).
     * @param count       Number of entries.
     * @param[out] record Filled with protocol metadata on success.
     * @return true if the signal matches the OOK pattern.
     */
    bool tryDecode(const int32_t *rawTimings,
                   size_t count,
                   SignalRecord &record) override;

    /**
     * @brief Encode a decoded OOK value into raw timings.
     *
     * @param record       Source signal record.
     * @param[out] timings Destination for pulse/gap array.
     * @param maxTimings   Destination capacity.
     * @return Number of timing entries written.
     */
    size_t encode(const SignalRecord &record,
                  int32_t *timings,
                  size_t maxTimings) override;

private:
    /// Default carrier frequency for 433 MHz OOK devices.
    static constexpr uint32_t DEFAULT_FREQ_HZ = 433920000U;

    /// Minimum gap duration (µs) that indicates a sync preamble.
    static constexpr int32_t SYNC_MIN_US = 4000;

    /// Nominal short-pulse duration (µs).
    static constexpr int32_t SHORT_PULSE_US = 350;

    /// Nominal long-pulse duration (µs).
    static constexpr int32_t LONG_PULSE_US = 1050;

    /// Tolerance factor (25 %).  A measured duration matches a reference
    /// when |measured - reference| < reference * TOLERANCE / 100.
    static constexpr int32_t TOLERANCE_PCT = 25;

    /// @brief Check whether @p measured is within tolerance of @p nominal.
    static bool matchesDuration(int32_t measured, int32_t nominal);
};

} // namespace hackos::radio
