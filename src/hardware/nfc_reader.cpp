#include "hardware/nfc_reader.h"

#include <cstring>
#include <esp_log.h>

#include "config.h"

static constexpr const char *TAG_NFC = "NFCReader";

// ── Well-known Mifare Classic default keys ──────────────────────────────────
const uint8_t NFCReader::DEFAULT_KEYS[NUM_DEFAULT_KEYS][6] = {
    {0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU}, // factory default
    {0xA0U, 0xA1U, 0xA2U, 0xA3U, 0xA4U, 0xA5U}, // MAD key A
    {0xB0U, 0xB1U, 0xB2U, 0xB3U, 0xB4U, 0xB5U}, // common alt key
    {0xD3U, 0xF7U, 0xD3U, 0xF7U, 0xD3U, 0xF7U}, // NFC Forum default
    {0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U}, // blank key
    {0x4DU, 0x3AU, 0x99U, 0xC3U, 0x51U, 0xDDU}, // NDEF key
};

NFCReader &NFCReader::instance()
{
    static NFCReader reader;
    return reader;
}

NFCReader::NFCReader()
    : nfc_(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI, PIN_NFC_CS),
      initialized_(false)
{
}

bool NFCReader::init()
{
    if (initialized_)
    {
        return true;
    }

    nfc_.begin();
    const uint32_t version = nfc_.getFirmwareVersion();
    if (version == 0U)
    {
        ESP_LOGE(TAG_NFC, "PN532 not found");
        return false;
    }

    ESP_LOGI(TAG_NFC, "PN532 found – firmware v%lu.%lu",
             static_cast<unsigned long>((version >> 16) & 0xFFUL),
             static_cast<unsigned long>((version >> 8) & 0xFFUL));

    nfc_.SAMConfig();
    initialized_ = true;
    return true;
}

void NFCReader::deinit()
{
    initialized_ = false;
    ESP_LOGI(TAG_NFC, "deinit");
}

bool NFCReader::isReady() const
{
    return initialized_;
}

bool NFCReader::readUID(uint8_t *uid, uint8_t *uidLen, uint16_t timeoutMs)
{
    if (!initialized_ || uid == nullptr || uidLen == nullptr)
    {
        return false;
    }

    return nfc_.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, uidLen,
                                    timeoutMs) == 1;
}

bool NFCReader::authenticateBlock(const uint8_t *uid, uint8_t uidLen, uint8_t blockAddr)
{
    if (!initialized_ || uid == nullptr)
    {
        return false;
    }

    // Copy key to mutable buffer – Adafruit_PN532 API takes non-const pointers
    uint8_t key[6];
    std::memcpy(key, DEFAULT_KEYS[0], 6U);

    // uid parameter also requires a non-const pointer per the Adafruit_PN532 API
    return nfc_.mifareclassic_AuthenticateBlock(
               const_cast<uint8_t *>(uid), uidLen, blockAddr, 0U, key) == 1;
}

bool NFCReader::authenticateBlockWithKeys(const uint8_t *uid, uint8_t uidLen,
                                          uint8_t blockAddr, uint8_t *keyIdx)
{
    if (!initialized_ || uid == nullptr)
    {
        return false;
    }

    for (uint8_t k = 0U; k < NUM_DEFAULT_KEYS; ++k)
    {
        // Need a fresh card selection before each auth attempt after a failure.
        // Re-select the card by doing a quick readPassiveTargetID if k > 0.
        if (k > 0U)
        {
            uint8_t tmpUid[7] = {};
            uint8_t tmpLen = 0U;
            if (!nfc_.readPassiveTargetID(PN532_MIFARE_ISO14443A, tmpUid, &tmpLen, 200U))
            {
                continue;
            }
        }

        uint8_t key[6];
        std::memcpy(key, DEFAULT_KEYS[k], 6U);

        if (nfc_.mifareclassic_AuthenticateBlock(
                const_cast<uint8_t *>(uid), uidLen, blockAddr, 0U, key) == 1)
        {
            if (keyIdx != nullptr)
            {
                *keyIdx = k;
            }
            ESP_LOGD(TAG_NFC, "Auth block %u OK with key #%u",
                     static_cast<unsigned>(blockAddr), static_cast<unsigned>(k));
            return true;
        }

        ESP_LOGD(TAG_NFC, "Auth block %u failed with key #%u",
                 static_cast<unsigned>(blockAddr), static_cast<unsigned>(k));
    }

    return false;
}

bool NFCReader::readBlock(uint8_t blockAddr, uint8_t *data)
{
    if (!initialized_ || data == nullptr)
    {
        return false;
    }

    return nfc_.mifareclassic_ReadDataBlock(blockAddr, data) == 1;
}

bool NFCReader::writeBlock(uint8_t blockAddr, const uint8_t *data)
{
    if (!initialized_ || data == nullptr)
    {
        return false;
    }

    return nfc_.mifareclassic_WriteDataBlock(blockAddr,
                                             const_cast<uint8_t *>(data)) == 1;
}

bool NFCReader::writeMagicUid(const uint8_t *newUid, uint8_t uidLen)
{
    if (!initialized_ || newUid == nullptr || (uidLen != 4U && uidLen != 7U))
    {
        return false;
    }

    // Step 1: Read the card to establish communication
    uint8_t cardUid[7] = {};
    uint8_t cardUidLen = 0U;
    if (!readUID(cardUid, &cardUidLen, 1000U))
    {
        ESP_LOGE(TAG_NFC, "writeMagicUid: no card found");
        return false;
    }

    // Step 2: Authenticate block 0 (manufacturer block) with default keys
    if (!authenticateBlockWithKeys(cardUid, cardUidLen, 0U, nullptr))
    {
        ESP_LOGE(TAG_NFC, "writeMagicUid: auth failed – may not be a Magic card");
        return false;
    }

    // Step 3: Read current block 0 to preserve SAK/ATQA/manufacturer data
    uint8_t block0[BYTES_PER_BLOCK] = {};
    if (!readBlock(0U, block0))
    {
        ESP_LOGE(TAG_NFC, "writeMagicUid: cannot read block 0");
        return false;
    }

    // Step 4: Overwrite UID bytes in block 0, recalculate BCC for 4-byte UID
    if (uidLen == 4U)
    {
        std::memcpy(block0, newUid, 4U);
        block0[4] = newUid[0] ^ newUid[1] ^ newUid[2] ^ newUid[3]; // BCC
    }
    else
    {
        // 7-byte UID occupies the first 7 bytes of block 0
        std::memcpy(block0, newUid, 7U);
    }

    // Step 5: Write the modified block 0 back
    if (!writeBlock(0U, block0))
    {
        ESP_LOGE(TAG_NFC, "writeMagicUid: write block 0 failed");
        return false;
    }

    ESP_LOGI(TAG_NFC, "writeMagicUid: UID written successfully");
    return true;
}

// ── NDEF URL builder ────────────────────────────────────────────────────────

size_t NFCReader::buildNdefUrl(const char *url, uint8_t prefixCode,
                               uint8_t *buf, size_t bufLen)
{
    if (url == nullptr || buf == nullptr)
    {
        return 0U;
    }

    const size_t urlLen = std::strlen(url);
    if (urlLen == 0U || urlLen > MAX_NDEF_URL_LEN)
    {
        return 0U;
    }

    // NDEF message:  [ NDEF record header | URI prefix | URL payload ]
    // Wrapped in a Type 2 Tag TLV: 0x03 <len> <ndef> 0xFE
    const size_t ndefRecordLen = 1U + urlLen;  // prefix byte + URL
    // NDEF record: flags(1) + type_length(1) + payload_length(1)
    //              + type(1) + payload(ndefRecordLen)
    const size_t totalNdef = 1U + 1U + 1U + 1U + ndefRecordLen;
    const size_t totalTlv = 1U + 1U + totalNdef + 1U; // 0x03 + len + ndef + 0xFE

    if (totalTlv > bufLen)
    {
        return 0U;
    }

    size_t pos = 0U;

    // TLV wrapper
    buf[pos++] = 0x03U;                                // NDEF Message TLV
    buf[pos++] = static_cast<uint8_t>(totalNdef);       // length of NDEF message

    // NDEF record header
    buf[pos++] = 0xD1U; // MB=1, ME=1, CF=0, SR=1, IL=0, TNF=0x01 (Well-Known)
    buf[pos++] = 0x01U; // Type length = 1
    buf[pos++] = static_cast<uint8_t>(ndefRecordLen); // Payload length
    buf[pos++] = 0x55U; // Type = 'U' (URI)

    // URI payload
    buf[pos++] = prefixCode; // URI identifier code
    std::memcpy(buf + pos, url, urlLen);
    pos += urlLen;

    // Terminator TLV
    buf[pos++] = 0xFEU;

    return pos;
}

bool NFCReader::emulateNtag213Url(const char *url, uint8_t prefixCode,
                                  uint16_t timeoutMs)
{
    if (!initialized_ || url == nullptr)
    {
        return false;
    }

    // Build the NDEF payload
    uint8_t ndefBuf[128] = {};
    const size_t ndefLen = buildNdefUrl(url, prefixCode, ndefBuf, sizeof(ndefBuf));
    if (ndefLen == 0U)
    {
        ESP_LOGE(TAG_NFC, "emulateNtag213Url: NDEF build failed");
        return false;
    }

    // PN532 target mode command parameters for NFC Forum Type 2 Tag emulation
    // SENS_RES (ATQA), NFCID1, SEL_RES (SAK)
    static const uint8_t atr[] = {
        0x04U, 0x04U, // SENS_RES (NTAG213-like)
        0x01U, 0x02U, 0x03U, // NFCID1t (3 bytes – PN532 fills rest)
        0x00U,        // SEL_RES (SAK = 0x00 for Type 2 Tag)
    };

    // Felica / DEP parameters (not used but required by API)
    static const uint8_t felicaParams[] = {
        0x01U, 0xFEU, 0xA2U, 0xA3U, 0xA4U, 0xA5U,
        0xA6U, 0xA7U, 0xC0U, 0xC1U, 0xC2U, 0xC3U,
        0xC4U, 0xC5U, 0xC6U, 0xC7U, 0xFFU, 0xFFU,
    };

    static const uint8_t nfcid3[] = {
        0x01U, 0xFEU, 0xA2U, 0xA3U, 0xA4U,
        0xA5U, 0xA6U, 0xA7U, 0xC0U, 0xC1U,
    };

    // Set PN532 as target (passive only, 106 kbps)
    const uint8_t activated = nfc_.tgInitAsTarget(
        atr, sizeof(atr),
        felicaParams, sizeof(felicaParams),
        nfcid3, sizeof(nfcid3),
        timeoutMs);

    if (activated == 0U)
    {
        ESP_LOGW(TAG_NFC, "emulateNtag213Url: no reader detected (timeout)");
        return false;
    }

    ESP_LOGI(TAG_NFC, "emulateNtag213Url: reader connected, serving NDEF");

    // Serve NFC Type 2 Tag read commands
    bool success = false;
    uint8_t cmd[32] = {};
    uint8_t cmdLen = 0U;

    for (uint8_t attempts = 0U; attempts < 20U; ++attempts)
    {
        cmdLen = sizeof(cmd);
        if (nfc_.tgGetData(cmd, &cmdLen) != 1)
        {
            break;
        }

        if (cmdLen < 2U)
        {
            continue;
        }

        // Type 2 Tag READ command: 0x30 <page>
        if (cmd[0] == 0x30U)
        {
            const uint8_t page = cmd[1];
            uint8_t resp[16] = {};

            // NTAG213 memory layout (simplified):
            // Pages 0-2: header (serial number, internal, lock, CC)
            // Pages 3+: user data (NDEF TLV)
            if (page <= 2U)
            {
                // Capability Container (CC): NDEF magic, version, size, RW
                resp[0] = 0x04U; // UID byte 1
                resp[1] = 0x01U; // UID byte 2
                resp[2] = 0x02U; // UID byte 3
                resp[3] = 0x03U; // Check byte

                if (page >= 1U)
                {
                    resp[4 - page * 4U] = 0x00U; // Internal
                }

                // CC bytes at page 3 (offset 12-15 of page 0 read)
                const size_t ccOff = (3U - page) * 4U;
                if (ccOff < 16U)
                {
                    resp[ccOff + 0U] = 0xE1U; // NDEF magic
                    resp[ccOff + 1U] = 0x10U; // Version 1.0
                    resp[ccOff + 2U] = 0x06U; // Size = 48 bytes
                    resp[ccOff + 3U] = 0x00U; // Read/write access
                }
            }
            else
            {
                // User data pages: serve NDEF content
                const size_t dataStart = (static_cast<size_t>(page) - 3U) * 4U;
                for (size_t i = 0U; i < 16U; ++i)
                {
                    const size_t idx = dataStart + i;
                    resp[i] = (idx < ndefLen) ? ndefBuf[idx] : 0x00U;
                }
            }

            if (nfc_.tgSetData(resp, 16U) == 1)
            {
                success = true;
            }
        }
        else
        {
            // Unknown command – respond with empty ACK
            uint8_t ack = 0x0AU;
            nfc_.tgSetData(&ack, 1U);
        }
    }

    ESP_LOGI(TAG_NFC, "emulateNtag213Url: %s", success ? "served" : "no reads");
    return success;
}
