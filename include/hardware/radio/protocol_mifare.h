/**
 * @file protocol_mifare.h
 * @brief NFC Mifare Classic protocol handler for the HackOS radio subsystem.
 *
 * Unlike OOK / NEC which deal with raw pulse timings, the Mifare protocol
 * operates at a higher level: the PN532 handles the RF modulation
 * internally and exposes block-level read/write access.
 *
 * This class translates PN532 data into the unified `SignalRecord` format
 * so that NFC card dumps can be stored and replayed using the same
 * pipeline as all other radio signals.
 *
 * The `tryDecode()` method interprets a raw byte buffer (the UID + data
 * blocks read from a card) and populates a SignalRecord.  `encode()`
 * converts a SignalRecord back into raw block data suitable for writing
 * to a blank card.
 */

#pragma once

#include "radio_protocol.h"

namespace hackos::radio {

/**
 * @brief Protocol handler for NFC Mifare Classic cards.
 */
class Protocol_Mifare : public RadioProtocol
{
public:
    Protocol_Mifare() = default;

    const char *name() const override { return "MIFARE"; }
    Modulation modulation() const override { return Modulation::NFC_A; }

    /**
     * @brief Interpret raw NFC block data as a Mifare record.
     *
     * @param rawTimings  Re-purposed: raw byte stream from PN532 read
     *                    (UID + sector data), cast to int32_t[].  Each
     *                    entry holds one byte in the low 8 bits.
     * @param count       Number of byte entries.
     * @param[out] record Filled with Mifare metadata on success.
     * @return true if the data looks like a valid Mifare dump.
     */
    bool tryDecode(const int32_t *rawTimings,
                   size_t count,
                   SignalRecord &record) override;

    /**
     * @brief Re-encode a Mifare SignalRecord into raw byte data.
     *
     * @param record       Source signal record.
     * @param[out] timings Destination for byte data (one byte per entry).
     * @param maxTimings   Destination capacity.
     * @return Number of byte entries written.
     */
    size_t encode(const SignalRecord &record,
                  int32_t *timings,
                  size_t maxTimings) override;

    // ── Mifare-specific constants ────────────────────────────────────────

    /// NFC carrier frequency (13.56 MHz).
    static constexpr uint32_t CARRIER_HZ = 13560000U;

    /// UID length for Mifare Classic 1K (4 bytes).
    static constexpr size_t UID_LEN_4 = 4U;

    /// UID length for Mifare Classic (7-byte variant).
    static constexpr size_t UID_LEN_7 = 7U;

    /// Maximum UID length.
    static constexpr size_t MAX_UID_LEN = 7U;

    /// Blocks per sector in Mifare Classic 1K.
    static constexpr size_t BLOCKS_PER_SECTOR = 4U;

    /// Total sectors in Mifare Classic 1K.
    static constexpr size_t SECTORS_1K = 16U;

    /// Bytes per data block.
    static constexpr size_t BYTES_PER_BLOCK = 16U;
};

} // namespace hackos::radio
