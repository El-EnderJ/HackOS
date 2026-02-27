/**
 * @file signal_format.h
 * @brief Universal signal storage format for HackOS radio subsystem.
 *
 * Defines `SignalRecord` – the canonical in-memory representation of any
 * captured or manually created radio signal.  The format is inspired by
 * the Flipper Zero `.sub` / `.ir` file layout but kept as a plain C++
 * struct so it can be serialised to the SD card or transmitted over USB
 * without external dependencies.
 *
 * Layout
 * ------
 * Every record carries:
 *  - **metadata** – frequency, modulation, protocol name, bit count, and
 *    a millisecond-resolution capture timestamp.
 *  - **raw timing data** – an array of signed 32-bit pulse/gap durations
 *    (positive = mark, negative = space) measured in microseconds.
 *  - **decoded value** – an optional 64-bit decoded payload for protocols
 *    that support it (e.g. NEC, RC5, OOK tri-state).
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include "irxtx_device.h"

namespace hackos::radio {

// ── Constants ────────────────────────────────────────────────────────────────

/// Maximum raw timing samples stored per record.
static constexpr size_t MAX_RAW_SAMPLES = 512U;

/// Maximum protocol name length (including null terminator).
static constexpr size_t MAX_PROTOCOL_NAME = 24U;

// ── SignalRecord ─────────────────────────────────────────────────────────────

/**
 * @brief Self-contained representation of a captured radio signal.
 *
 * This struct is designed to be trivially copyable so that it can be
 * stored in a RingBuffer, written to SD, or passed through a FreeRTOS
 * MessageBuffer without extra serialisation.
 */
struct SignalRecord
{
    // ── Metadata ─────────────────────────────────────────────────────────

    /// Carrier / centre frequency in Hz (e.g. 433920000, 38000 for IR).
    uint32_t frequencyHz;

    /// Modulation scheme used during capture.
    Modulation modulation;

    /// Human-readable protocol name (NUL-terminated).
    char protocolName[MAX_PROTOCOL_NAME];

    /// Number of significant bits in decodedValue (0 = raw-only).
    uint16_t bitCount;

    /// Decoded payload when a protocol parser matched.
    uint64_t decodedValue;

    /// Capture timestamp (millis() at the moment the record was created).
    uint32_t timestampMs;

    // ── Raw timing data ──────────────────────────────────────────────────

    /// Pulse/gap durations in microseconds (positive = mark, negative = space).
    int32_t rawTimings[MAX_RAW_SAMPLES];

    /// Number of valid entries in rawTimings[].
    uint16_t rawCount;

    // ── Helpers ──────────────────────────────────────────────────────────

    /// @brief Zero-initialise the record.
    void clear()
    {
        frequencyHz = 0U;
        modulation = Modulation::OOK;
        protocolName[0] = '\0';
        bitCount = 0U;
        decodedValue = 0ULL;
        timestampMs = 0U;
        rawCount = 0U;
        for (size_t i = 0U; i < MAX_RAW_SAMPLES; ++i)
        {
            rawTimings[i] = 0;
        }
    }

    /// @brief Returns true when the record carries decoded protocol data.
    bool isDecoded() const
    {
        return bitCount > 0U;
    }

    /// @brief Returns true when the record carries raw timing data.
    bool hasRaw() const
    {
        return rawCount > 0U;
    }
};

} // namespace hackos::radio
