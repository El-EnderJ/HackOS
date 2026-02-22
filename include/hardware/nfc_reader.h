/**
 * @file nfc_reader.h
 * @brief HAL wrapper for PN532 NFC/RFID using Adafruit_PN532 (SPI).
 */

#pragma once

#include <Adafruit_PN532.h>
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
     * @brief Read 16 bytes from an already-authenticated block.
     * @param blockAddr Absolute block address.
     * @param data      Output buffer of 16 bytes.
     */
    bool readBlock(uint8_t blockAddr, uint8_t *data);

private:
    static const uint8_t DEFAULT_KEY_A[6];

    NFCReader();

    Adafruit_PN532 nfc_;
    bool initialized_;
};
