#include "hardware/ir_transceiver.h"

#include <IRutils.h>
#include <esp_log.h>
#include <new>

#include "config.h"

static constexpr const char *TAG_IR = "IRTransceiver";

IRTransceiver &IRTransceiver::instance()
{
    static IRTransceiver ir;
    return ir;
}

IRTransceiver::IRTransceiver()
    : recv_(nullptr),
      send_(nullptr),
      results_{},
      recvActive_(false),
      sendActive_(false),
      hasLast_(false),
      lastValue_(0U),
      lastProtocol_(decode_type_t::UNKNOWN),
      lastBits_(0U)
{
}

bool IRTransceiver::initReceive()
{
    if (recvActive_)
    {
        return true;
    }

    recv_ = new (std::nothrow) IRrecv(PIN_IR_RX, CAPTURE_BUF, TIMEOUT_MS, true);
    if (recv_ == nullptr)
    {
        ESP_LOGE(TAG_IR, "OOM allocating IRrecv");
        return false;
    }

    recv_->enableIRIn();
    recvActive_ = true;
    ESP_LOGI(TAG_IR, "IR receiver enabled on pin %u", static_cast<unsigned>(PIN_IR_RX));
    return true;
}

bool IRTransceiver::initTransmit()
{
    if (sendActive_)
    {
        return true;
    }

    send_ = new (std::nothrow) IRsend(PIN_IR_TX);
    if (send_ == nullptr)
    {
        ESP_LOGE(TAG_IR, "OOM allocating IRsend");
        return false;
    }

    send_->begin();
    sendActive_ = true;
    ESP_LOGI(TAG_IR, "IR transmitter enabled on pin %u", static_cast<unsigned>(PIN_IR_TX));
    return true;
}

void IRTransceiver::deinit()
{
    if (recvActive_ && recv_ != nullptr)
    {
        recv_->disableIRIn();
    }
    delete recv_;
    recv_ = nullptr;
    delete send_;
    send_ = nullptr;
    recvActive_ = false;
    sendActive_ = false;
    // Note: hasLast_, lastValue_, lastProtocol_, lastBits_ intentionally
    // preserved so the Cloner can send the last captured code after re-init.
    ESP_LOGI(TAG_IR, "deinit");
}

bool IRTransceiver::decode(uint64_t &value, decode_type_t &protocol, uint16_t &bits)
{
    if (!recvActive_ || recv_ == nullptr)
    {
        return false;
    }

    if (!recv_->decode(&results_))
    {
        return false;
    }

    value = results_.value;
    protocol = results_.decode_type;
    bits = static_cast<uint16_t>(results_.bits);

    lastValue_ = value;
    lastProtocol_ = protocol;
    lastBits_ = bits;
    hasLast_ = true;

    recv_->resume();
    ESP_LOGD(TAG_IR, "decoded: proto=%d value=0x%llX bits=%u",
             static_cast<int>(protocol),
             static_cast<unsigned long long>(value),
             static_cast<unsigned>(bits));
    return true;
}

void IRTransceiver::send(uint64_t value, decode_type_t protocol, uint16_t bits)
{
    if (!sendActive_ || send_ == nullptr)
    {
        return;
    }

    send_->send(protocol, value, bits);
    ESP_LOGI(TAG_IR, "sent: proto=%d value=0x%llX bits=%u",
             static_cast<int>(protocol),
             static_cast<unsigned long long>(value),
             static_cast<unsigned>(bits));
}

bool IRTransceiver::hasLastCode() const
{
    return hasLast_;
}

uint64_t IRTransceiver::lastValue() const
{
    return lastValue_;
}

decode_type_t IRTransceiver::lastProtocol() const
{
    return lastProtocol_;
}

uint16_t IRTransceiver::lastBits() const
{
    return lastBits_;
}
