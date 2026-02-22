#include "hardware/nfc_reader.h"

#include <esp_log.h>

#include "config.h"

static constexpr const char *TAG_NFC = "NFCReader";

const uint8_t NFCReader::DEFAULT_KEY_A[6] = {0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU};

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
    for (uint8_t i = 0U; i < 6U; ++i)
    {
        key[i] = DEFAULT_KEY_A[i];
    }

    // uid parameter also requires a non-const pointer per the Adafruit_PN532 API
    return nfc_.mifareclassic_AuthenticateBlock(
               const_cast<uint8_t *>(uid), uidLen, blockAddr, 0U, key) == 1;
}

bool NFCReader::readBlock(uint8_t blockAddr, uint8_t *data)
{
    if (!initialized_ || data == nullptr)
    {
        return false;
    }

    return nfc_.mifareclassic_ReadDataBlock(blockAddr, data) == 1;
}
