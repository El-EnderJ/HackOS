#include "hardware/rf_transceiver.h"

#include <Arduino.h>
#include <esp_log.h>

#include "config.h"

static constexpr const char *TAG_RF = "RFTransceiver";

RFTransceiver &RFTransceiver::instance()
{
    static RFTransceiver rf;
    return rf;
}

RFTransceiver::RFTransceiver()
    : rf_(),
      recvActive_(false),
      sendActive_(false),
      hasLast_(false),
      lastCode_(0UL),
      lastBitLength_(0U),
      lastDelay_(0U)
{
}

bool RFTransceiver::initReceive()
{
    if (recvActive_)
    {
        return true;
    }

    rf_.enableReceive(digitalPinToInterrupt(PIN_RF_RX));
    recvActive_ = true;
    ESP_LOGI(TAG_RF, "RF receiver enabled on pin %u", static_cast<unsigned>(PIN_RF_RX));
    return true;
}

bool RFTransceiver::initTransmit()
{
    if (sendActive_)
    {
        return true;
    }

    rf_.enableTransmit(PIN_RF_TX);
    sendActive_ = true;
    ESP_LOGI(TAG_RF, "RF transmitter enabled on pin %u", static_cast<unsigned>(PIN_RF_TX));
    return true;
}

void RFTransceiver::deinit()
{
    if (recvActive_)
    {
        rf_.disableReceive();
    }
    // RCSwitch has no disableTransmit() – simply clear the flag
    recvActive_ = false;
    sendActive_ = false;
    // Note: hasLast_, lastCode_, lastBitLength_, lastDelay_ intentionally
    // preserved so the Transmitter can re-send the last captured code.
    ESP_LOGI(TAG_RF, "deinit");
}

bool RFTransceiver::available()
{
    return recvActive_ && rf_.available();
}

bool RFTransceiver::read(unsigned long &code, unsigned int &bitLength, unsigned int &delay)
{
    if (!recvActive_ || !rf_.available())
    {
        return false;
    }

    code = rf_.getReceivedValue();
    bitLength = rf_.getReceivedBitlength();
    delay = rf_.getReceivedDelay();

    lastCode_ = code;
    lastBitLength_ = bitLength;
    lastDelay_ = delay;
    hasLast_ = true;

    rf_.resetAvailable();
    ESP_LOGD(TAG_RF, "received: code=%lu bits=%u delay=%u",
             code, static_cast<unsigned>(bitLength), static_cast<unsigned>(delay));
    return true;
}

void RFTransceiver::send(unsigned long code, unsigned int bitLength)
{
    if (!sendActive_)
    {
        return;
    }

    rf_.send(code, bitLength);
    ESP_LOGI(TAG_RF, "sent: code=%lu bits=%u", code, static_cast<unsigned>(bitLength));
}

bool RFTransceiver::hasLastCode() const
{
    return hasLast_;
}

unsigned long RFTransceiver::lastCode() const
{
    return lastCode_;
}

unsigned int RFTransceiver::lastBitLength() const
{
    return lastBitLength_;
}

unsigned int RFTransceiver::lastDelay() const
{
    return lastDelay_;
}
