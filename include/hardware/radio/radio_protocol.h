/**
 * @file radio_protocol.h
 * @brief Base class for radio protocol parsers and protocol registry.
 *
 * Architecture overview
 * ---------------------
 * `RadioProtocol` is the abstract base for every decodable signal
 * protocol.  Concrete implementations (Protocol_OOK, Protocol_NEC,
 * Protocol_Mifare) override `tryDecode()` and `encode()`.
 *
 * `ProtocolRegistry` is a lightweight factory that keeps a static array
 * of protocol pointers.  During boot each protocol registers itself so
 * the RadioManager can iterate over all known protocols when an unknown
 * signal arrives.
 *
 * Adding a new protocol
 * ---------------------
 * 1. Subclass RadioProtocol.
 * 2. Call `ProtocolRegistry::registerProtocol(&myProto)` at startup.
 * 3. The RadioManager worker will automatically try the new parser.
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include "signal_format.h"

namespace hackos::radio {

// ── RadioProtocol (abstract base) ────────────────────────────────────────────

/**
 * @brief Abstract base class for all radio signal protocol parsers.
 *
 * A protocol knows how to:
 *  - **decode** raw timing data into a SignalRecord with metadata.
 *  - **encode** a decoded value back into raw timings for replay.
 */
class RadioProtocol
{
public:
    virtual ~RadioProtocol() = default;

    /// @brief Human-readable protocol identifier (e.g. "OOK", "NEC").
    virtual const char *name() const = 0;

    /// @brief The modulation type this protocol operates on.
    virtual Modulation modulation() const = 0;

    /**
     * @brief Attempt to decode raw timing data.
     *
     * @param rawTimings  Array of pulse/gap durations (µs).
     * @param count       Number of entries in @p rawTimings.
     * @param[out] record Filled on success with decoded metadata.
     * @return true if this protocol matched the signal.
     */
    virtual bool tryDecode(const int32_t *rawTimings,
                           size_t count,
                           SignalRecord &record) = 0;

    /**
     * @brief Encode a decoded value into raw timing data for replay.
     *
     * @param record        The signal record to encode.
     * @param[out] timings  Destination array for pulse/gap durations (µs).
     * @param maxTimings    Maximum entries that fit in @p timings.
     * @return Number of timing entries written (0 on failure).
     */
    virtual size_t encode(const SignalRecord &record,
                          int32_t *timings,
                          size_t maxTimings) = 0;
};

// ── ProtocolRegistry (plugin factory) ────────────────────────────────────────

/**
 * @brief Static registry for protocol parser plugins.
 *
 * The registry holds up to MAX_PROTOCOLS protocol pointers.  Protocols
 * are registered once at boot and remain valid for the lifetime of the
 * process.
 */
class ProtocolRegistry
{
public:
    /// Maximum number of simultaneously registered protocols.
    static constexpr size_t MAX_PROTOCOLS = 16U;

    /**
     * @brief Register a protocol parser.
     * @param proto  Pointer to a protocol instance (must outlive the registry).
     * @return true on success, false if the registry is full.
     */
    static bool registerProtocol(RadioProtocol *proto);

    /// @brief Number of currently registered protocols.
    static size_t count();

    /**
     * @brief Retrieve a registered protocol by index.
     * @return Pointer to the protocol, or nullptr if index is out of range.
     */
    static RadioProtocol *get(size_t index);

    /**
     * @brief Look up a protocol by name (case-sensitive).
     * @return Pointer to the protocol, or nullptr if not found.
     */
    static RadioProtocol *findByName(const char *name);

    /**
     * @brief Try all registered protocols against the raw timings.
     *
     * Iterates through every registered protocol and returns the first
     * one that successfully decodes the signal.
     *
     * @param rawTimings  Array of pulse/gap durations (µs).
     * @param count       Number of entries.
     * @param[out] record Filled with decoded data on match.
     * @return Pointer to the matching protocol, or nullptr.
     */
    static RadioProtocol *tryDecodeAll(const int32_t *rawTimings,
                                       size_t count,
                                       SignalRecord &record);

private:
    ProtocolRegistry() = delete;

    static RadioProtocol *protocols_[MAX_PROTOCOLS];
    static size_t count_;
};

} // namespace hackos::radio
