/**
 * @file nfc_reader.h
 * @brief HAL wrapper for PN532 NFC/RFID using Adafruit_PN532 (SPI).
 *
 * Phase 11 additions:
 *  - Multiple default-key authentication for Mifare Classic sector dumps.
 *  - Magic-card (Gen1/Gen2) UID writing.
 *  - NTAG213 URL tag emulation via PN532 Target mode.
 *
 * Phase 12 additions:
 *  - Full NTAG215 (540-byte) Amiibo emulation via tgInitAsTarget.
 *  - NTAG215 binary dump writing to blank physical tags.
 */

#pragma once

#include <Adafruit_PN532.h>
#include <cstddef>
#include <cstdint>

class NFCReader
{
public:
    /// Number of sectors in a Mifare Classic 1K card.
    static constexpr uint8_t MIFARE_1K_SECTORS = 16U;
    /// Number of blocks per sector in Mifare Classic 1K.
    static constexpr uint8_t BLOCKS_PER_SECTOR = 4U;
    /// Bytes per data block.
    static constexpr uint8_t BYTES_PER_BLOCK = 16U;
    /// Total blocks in Mifare Classic 1K.
    static constexpr uint8_t MIFARE_1K_BLOCKS = MIFARE_1K_SECTORS * BLOCKS_PER_SECTOR;
    /// Number of well-known default keys to try during sector dumps.
    static constexpr uint8_t NUM_DEFAULT_KEYS = 6U;
    /// Maximum NDEF URL payload length for NTAG213 emulation.
    static constexpr uint8_t MAX_NDEF_URL_LEN = 64U;

    static NFCReader &instance();

    bool init();
    void deinit();
    bool isReady() const;

    /**
     * @brief Attempt to read the UID of a passive ISO14443A card.
     * @param uid   Buffer of at least 7 bytes to receive the UID.
     * @param uidLen Receives the actual UID length (4 or 7).
     * @param timeoutMs Milliseconds to wait for a card.
     * @return true on success.
     */
    bool readUID(uint8_t *uid, uint8_t *uidLen, uint16_t timeoutMs = 500U);

    /**
     * @brief Authenticate a Mifare Classic block with default key A (0xFF*6).
     * @param uid    Card UID.
     * @param uidLen Card UID length.
     * @param blockAddr Absolute block address (0–63 for 1K).
     */
    bool authenticateBlock(const uint8_t *uid, uint8_t uidLen, uint8_t blockAddr);

    /**
     * @brief Authenticate a block trying all well-known default keys.
     * @param uid       Card UID.
     * @param uidLen    Card UID length.
     * @param blockAddr Absolute block address.
     * @param[out] keyIdx  If non-null, receives the index of the key that
     *                     succeeded (0..NUM_DEFAULT_KEYS-1).
     * @return true if any key succeeded.
     */
    bool authenticateBlockWithKeys(const uint8_t *uid, uint8_t uidLen,
                                   uint8_t blockAddr, uint8_t *keyIdx = nullptr);

    /**
     * @brief Read 16 bytes from an already-authenticated block.
     * @param blockAddr Absolute block address.
     * @param data      Output buffer of 16 bytes.
     */
    bool readBlock(uint8_t blockAddr, uint8_t *data);

    /**
     * @brief Write 16 bytes to an already-authenticated block.
     * @param blockAddr Absolute block address.
     * @param data      Input buffer of 16 bytes.
     * @return true on success.
     */
    bool writeBlock(uint8_t blockAddr, const uint8_t *data);

    /**
     * @brief Write a UID to a Mifare Classic "Magic" Gen1/Gen2 card.
     *
     * Gen1 cards allow writing block 0 after a special backdoor auth.
     * This method authenticates block 0 with default keys and writes
     * the new UID into the manufacturer block.
     *
     * @param newUid  New UID bytes (4 bytes for classic).
     * @param uidLen  Length of the UID (4 or 7).
     * @return true on success.
     */
    bool writeMagicUid(const uint8_t *newUid, uint8_t uidLen);

    /**
     * @brief Emulate an NTAG213-style NFC Forum Type 2 tag with a URL.
     *
     * Uses the PN532's TgInitAsTarget mode to present an NDEF message
     * containing a URL record to any NFC-enabled phone.  Blocks until
     * the reader interaction finishes or the timeout expires.
     *
     * @param url        Null-terminated URL string (without scheme prefix).
     * @param prefixCode NDEF URI prefix code (0x04 = "https://", etc.).
     * @param timeoutMs  Maximum time to stay in target mode.
     * @return true if at least one successful exchange occurred.
     */
    bool emulateNtag213Url(const char *url, uint8_t prefixCode = 0x04U,
                           uint16_t timeoutMs = 30000U);

    // ── Phase 12: NTAG215 Amiibo emulation & writing ────────────────────

    /// Size of an NTAG215 dump in bytes (135 pages × 4 bytes).
    static constexpr size_t NTAG215_SIZE = 540U;
    /// Number of 4-byte pages in an NTAG215 tag.
    static constexpr uint8_t NTAG215_PAGES = 135U;

    /**
     * @brief Emulate a full NTAG215 tag from a raw 540-byte binary dump.
     *
     * Configures the PN532 in tgInitAsTarget mode and serves page-read
     * commands from the dump buffer.  Handles READ (0x30) and PWD_AUTH
     * (0x1B) commands.  Blocks until the reader disconnects or the
     * timeout expires.
     *
     * @param dump      Pointer to a 540-byte NTAG215 binary image.
     * @param timeoutMs Maximum time to stay in target mode.
     * @return true if at least one successful exchange occurred.
     */
    bool emulateNtag215(const uint8_t *dump, uint16_t timeoutMs = 30000U);

    /**
     * @brief Write a 540-byte binary dump to a physical blank NTAG215 tag.
     *
     * Writes pages 4–129 (user data area) of the dump to the tag.
     * Pages 0–3 (UID/manufacturer) and 130–134 (config/password) are
     * skipped as they are typically read-only or require special auth.
     *
     * @param dump Pointer to a 540-byte NTAG215 binary image.
     * @return true if all writable pages were written successfully.
     */
    bool writeNtag215(const uint8_t *dump);

    /// @brief Access the table of default keys (6 bytes each).
    static const uint8_t (*defaultKeys())[6] { return DEFAULT_KEYS; }

private:
    static const uint8_t DEFAULT_KEYS[NUM_DEFAULT_KEYS][6];

    NFCReader();

    /// Build an NDEF Type-2 Tag TLV payload with a single URI record.
    static size_t buildNdefUrl(const char *url, uint8_t prefixCode,
                               uint8_t *buf, size_t bufLen);

    Adafruit_PN532 nfc_;
    bool initialized_;
};
