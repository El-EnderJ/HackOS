#include "hardware/nfc_reader.h"

#include <SPI.h>
#include <cstring>
#include <esp_log.h>

#include "config.h"

// ── SPI Transaction Guard for shared bus ────────────────────────────────────
static const SPISettings NFC_SPI_SETTINGS(1000000, LSBFIRST, SPI_MODE0);

/// @brief Helper: send data in PN532 target/emulation mode.
/// Wraps setDataTarget with the required 0x8E (TgSetData) command prefix.
static bool sendTargetResponse(Adafruit_PN532 &nfc, uint8_t *data, uint8_t len)
{
    // setDataTarget expects cmd[0] == 0x8E (TgSetData command code)
    uint8_t buf[64];
    if (static_cast<size_t>(len) + 1U > sizeof(buf))
    {
        return false;
    }
    buf[0] = 0x8EU;
    std::memcpy(buf + 1, data, len);
    return nfc.setDataTarget(buf, static_cast<uint8_t>(len + 1U)) == 1;
}

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

    SPI.beginTransaction(NFC_SPI_SETTINGS);
    nfc_.begin();
    const uint32_t version = nfc_.getFirmwareVersion();
    SPI.endTransaction();

    if (version == 0U)
    {
        ESP_LOGE(TAG_NFC, "PN532 not found");
        return false;
    }

    ESP_LOGI(TAG_NFC, "PN532 found – firmware v%lu.%lu",
             static_cast<unsigned long>((version >> 16) & 0xFFUL),
             static_cast<unsigned long>((version >> 8) & 0xFFUL));

    SPI.beginTransaction(NFC_SPI_SETTINGS);
    nfc_.SAMConfig();
    SPI.endTransaction();
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

    SPI.beginTransaction(NFC_SPI_SETTINGS);
    const bool ok = nfc_.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, uidLen,
                                    timeoutMs) == 1;
    SPI.endTransaction();
    return ok;
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
    SPI.beginTransaction(NFC_SPI_SETTINGS);
    const bool ok = nfc_.mifareclassic_AuthenticateBlock(
               const_cast<uint8_t *>(uid), uidLen, blockAddr, 0U, key) == 1;
    SPI.endTransaction();
    return ok;
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
            SPI.beginTransaction(NFC_SPI_SETTINGS);
            const bool reselected = nfc_.readPassiveTargetID(
                PN532_MIFARE_ISO14443A, tmpUid, &tmpLen, 200U);
            SPI.endTransaction();
            if (!reselected)
            {
                continue;
            }
        }

        uint8_t key[6];
        std::memcpy(key, DEFAULT_KEYS[k], 6U);

        SPI.beginTransaction(NFC_SPI_SETTINGS);
        const bool authOk = nfc_.mifareclassic_AuthenticateBlock(
                const_cast<uint8_t *>(uid), uidLen, blockAddr, 0U, key) == 1;
        SPI.endTransaction();

        if (authOk)
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

    SPI.beginTransaction(NFC_SPI_SETTINGS);
    const bool ok = nfc_.mifareclassic_ReadDataBlock(blockAddr, data) == 1;
    SPI.endTransaction();
    return ok;
}

bool NFCReader::writeBlock(uint8_t blockAddr, const uint8_t *data)
{
    if (!initialized_ || data == nullptr)
    {
        return false;
    }

    SPI.beginTransaction(NFC_SPI_SETTINGS);
    const bool ok = nfc_.mifareclassic_WriteDataBlock(blockAddr,
                                             const_cast<uint8_t *>(data)) == 1;
    SPI.endTransaction();
    return ok;
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
    SPI.beginTransaction(NFC_SPI_SETTINGS);
    const uint8_t activated = nfc_.AsTarget();
    SPI.endTransaction();

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
        SPI.beginTransaction(NFC_SPI_SETTINGS);
        const bool gotData = nfc_.getDataTarget(cmd, &cmdLen) == 1;
        SPI.endTransaction();
        if (!gotData)
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
            // Page 0-2: header (serial number, internal, lock)
            // Page 3:   Capability Container (CC)
            // Pages 4+: user data (NDEF TLV)
            //
            // A Type 2 Tag READ command returns 16 bytes (4 pages)
            // starting from the requested page number.
            if (page <= 3U)
            {
                // Build a flat 16-byte header image for pages 0-3
                const uint8_t header[16] = {
                    0x04U, 0x01U, 0x02U, 0x03U, // Page 0: UID bytes + BCC
                    0x04U, 0x05U, 0x06U, 0x07U, // Page 1: UID bytes + BCC
                    0x00U, 0x00U, 0x00U, 0x00U, // Page 2: Internal + Lock
                    0xE1U, 0x10U, 0x06U, 0x00U, // Page 3: CC (NDEF magic)
                };

                // Copy 16 bytes starting from the requested page
                const size_t startByte = static_cast<size_t>(page) * 4U;
                for (size_t i = 0U; i < 16U; ++i)
                {
                    const size_t idx = startByte + i;
                    resp[i] = (idx < sizeof(header)) ? header[idx] : 0x00U;
                }
            }
            else
            {
                // User data pages: serve NDEF content (starts at page 4)
                const size_t dataStart = (static_cast<size_t>(page) - 4U) * 4U;
                for (size_t i = 0U; i < 16U; ++i)
                {
                    const size_t idx = dataStart + i;
                    resp[i] = (idx < ndefLen) ? ndefBuf[idx] : 0x00U;
                }
            }

            SPI.beginTransaction(NFC_SPI_SETTINGS);
            const bool sent = sendTargetResponse(nfc_, resp, 16U);
            SPI.endTransaction();
            if (sent)
            {
                success = true;
            }
        }
        else
        {
            // Unknown command – respond with empty ACK
            uint8_t ack = 0x0AU;
            SPI.beginTransaction(NFC_SPI_SETTINGS);
            sendTargetResponse(nfc_, &ack, 1U);
            SPI.endTransaction();
        }
    }

    ESP_LOGI(TAG_NFC, "emulateNtag213Url: %s", success ? "served" : "no reads");
    return success;
}

// ── NTAG215 full-dump Amiibo emulation ──────────────────────────────────────

bool NFCReader::emulateNtag215(const uint8_t *dump, uint16_t timeoutMs)
{
    if (!initialized_ || dump == nullptr)
    {
        return false;
    }

    // Extract UID bytes from the dump to use in SENS_RES/NFCID1
    // NTAG215 page 0: UID0 UID1 UID2 BCC0, page 1: UID3 UID4 UID5 UID6
    const uint8_t atr[] = {
        0x44U, 0x00U,                         // SENS_RES (NTAG215)
        dump[0], dump[1], dump[2],             // NFCID1t (3 bytes)
        0x00U,                                 // SEL_RES (SAK = 0x00, Type 2 Tag)
    };

    static const uint8_t felicaParams[] = {
        0x01U, 0xFEU, 0xA2U, 0xA3U, 0xA4U, 0xA5U,
        0xA6U, 0xA7U, 0xC0U, 0xC1U, 0xC2U, 0xC3U,
        0xC4U, 0xC5U, 0xC6U, 0xC7U, 0xFFU, 0xFFU,
    };

    static const uint8_t nfcid3[] = {
        0x01U, 0xFEU, 0xA2U, 0xA3U, 0xA4U,
        0xA5U, 0xA6U, 0xA7U, 0xC0U, 0xC1U,
    };

    // Suppress unused variable warnings – params are kept for reference
    // but AsTarget() uses its own hardcoded configuration
    (void)atr;
    (void)felicaParams;
    (void)nfcid3;
    (void)timeoutMs;

    SPI.beginTransaction(NFC_SPI_SETTINGS);
    const uint8_t activated = nfc_.AsTarget();
    SPI.endTransaction();

    if (activated == 0U)
    {
        ESP_LOGW(TAG_NFC, "emulateNtag215: no reader detected (timeout)");
        return false;
    }

    ESP_LOGI(TAG_NFC, "emulateNtag215: reader connected, serving pages");

    bool success = false;
    uint8_t cmd[32] = {};
    uint8_t cmdLen = 0U;

    for (uint8_t attempts = 0U; attempts < 50U; ++attempts)
    {
        cmdLen = sizeof(cmd);
        SPI.beginTransaction(NFC_SPI_SETTINGS);
        const bool gotData = nfc_.getDataTarget(cmd, &cmdLen) == 1;
        SPI.endTransaction();
        if (!gotData)
        {
            break;
        }

        if (cmdLen < 1U)
        {
            continue;
        }

        if (cmd[0] == 0x30U && cmdLen >= 2U)
        {
            // READ command: 0x30 <page> → return 16 bytes (4 pages)
            const uint8_t page = cmd[1];
            uint8_t resp[16] = {};

            for (uint8_t i = 0U; i < 4U; ++i)
            {
                const uint8_t p = page + i;
                if (p < NTAG215_PAGES)
                {
                    std::memcpy(resp + (i * 4U), dump + (static_cast<size_t>(p) * 4U), 4U);
                }
            }

            SPI.beginTransaction(NFC_SPI_SETTINGS);
            const bool sent = sendTargetResponse(nfc_, resp, 16U);
            SPI.endTransaction();
            if (sent)
            {
                success = true;
            }
        }
        else if (cmd[0] == 0x60U && cmdLen >= 2U)
        {
            // GET_VERSION command → respond with NTAG215 version info
            uint8_t version[] = {
                0x00U, 0x04U, 0x04U, 0x02U,
                0x01U, 0x00U, 0x11U, 0x03U,
            };
            SPI.beginTransaction(NFC_SPI_SETTINGS);
            sendTargetResponse(nfc_, version, sizeof(version));
            SPI.endTransaction();
        }
        else if (cmd[0] == 0x1BU && cmdLen >= 5U)
        {
            // PWD_AUTH command: 0x1B <pw0> <pw1> <pw2> <pw3>
            // Respond with PACK (2 bytes) from pages 133-134 area
            // For Amiibo, respond with 0x80 0x80 (standard PACK)
            uint8_t pack[] = {0x80U, 0x80U};
            SPI.beginTransaction(NFC_SPI_SETTINGS);
            sendTargetResponse(nfc_, pack, sizeof(pack));
            SPI.endTransaction();
            success = true;
        }
        else if (cmd[0] == 0x3AU && cmdLen >= 3U)
        {
            // FAST_READ: 0x3A <startPage> <endPage> → return all pages
            const uint8_t startPage = cmd[1];
            const uint8_t endPage = cmd[2];
            if (startPage <= endPage && endPage < NTAG215_PAGES)
            {
                const uint8_t count = endPage - startPage + 1U;
                // Limit response to avoid buffer overflow (max ~60 bytes safe)
                const uint8_t maxPages = 14U; // 56 bytes, safe for PN532 buffer
                const uint8_t pages = (count > maxPages) ? maxPages : count;
                uint8_t resp[56] = {};
                std::memcpy(resp, dump + (static_cast<size_t>(startPage) * 4U),
                            static_cast<size_t>(pages) * 4U);
                SPI.beginTransaction(NFC_SPI_SETTINGS);
                sendTargetResponse(nfc_, resp, pages * 4U);
                SPI.endTransaction();
                success = true;
            }
            else
            {
                uint8_t nack = 0x00U;
                SPI.beginTransaction(NFC_SPI_SETTINGS);
                sendTargetResponse(nfc_, &nack, 1U);
                SPI.endTransaction();
            }
        }
        else
        {
            // Unknown command – respond with ACK
            uint8_t ack = 0x0AU;
            SPI.beginTransaction(NFC_SPI_SETTINGS);
            sendTargetResponse(nfc_, &ack, 1U);
            SPI.endTransaction();
        }
    }

    ESP_LOGI(TAG_NFC, "emulateNtag215: %s", success ? "served" : "no reads");
    return success;
}

// ── NTAG215 binary dump writer ──────────────────────────────────────────────

bool NFCReader::writeNtag215(const uint8_t *dump)
{
    if (!initialized_ || dump == nullptr)
    {
        return false;
    }

    // Step 1: Detect the tag
    uint8_t uid[7] = {};
    uint8_t uidLen = 0U;
    if (!readUID(uid, &uidLen, 2000U))
    {
        ESP_LOGE(TAG_NFC, "writeNtag215: no tag found");
        return false;
    }

    // Step 2: Write user-data pages (4–129)
    // Pages 0-3 are manufacturer/UID (read-only on real tags)
    // Pages 130-134 are config/password area
    for (uint8_t page = 4U; page < 130U; ++page)
    {
        const size_t offset = static_cast<size_t>(page) * 4U;
        uint8_t pageData[4];
        std::memcpy(pageData, dump + offset, 4U);

        SPI.beginTransaction(NFC_SPI_SETTINGS);
        const bool writeOk = nfc_.ntag2xx_WritePage(page, pageData) == 1;
        SPI.endTransaction();

        if (!writeOk)
        {
            ESP_LOGE(TAG_NFC, "writeNtag215: write page %u failed",
                     static_cast<unsigned>(page));
            return false;
        }

        ESP_LOGD(TAG_NFC, "writeNtag215: page %u OK", static_cast<unsigned>(page));
    }

    ESP_LOGI(TAG_NFC, "writeNtag215: all pages written successfully");
    return true;
}
