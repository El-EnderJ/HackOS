#include "hardware/radio/radio_manager.h"

#include <cstring>

#include "hardware/radio/radio_protocol.h"

namespace hackos::radio {

RadioManager &RadioManager::instance()
{
    static RadioManager mgr;
    return mgr;
}

RadioManager::RadioManager()
    : devices_{},
      deviceCount_(0U),
      activeDevice_(nullptr),
      rawRing_(),
      workerBuf_{},
      workerBufLen_(0U),
      txBuf_{},
      readBuf_{},
      lastRecord_{},
      hasLast_(false)
{
    lastRecord_.clear();
}

// ── Device registration ──────────────────────────────────────────────────────

bool RadioManager::registerDevice(IRxTxDevice *device)
{
    if (device == nullptr || deviceCount_ >= MAX_DEVICES)
    {
        return false;
    }
    devices_[deviceCount_++] = device;
    return true;
}

IRxTxDevice *RadioManager::findDevice(const char *name) const
{
    if (name == nullptr)
    {
        return nullptr;
    }
    for (size_t i = 0U; i < deviceCount_; ++i)
    {
        if (std::strcmp(devices_[i]->name(), name) == 0)
        {
            return devices_[i];
        }
    }
    return nullptr;
}

// ── Capture control ──────────────────────────────────────────────────────────

bool RadioManager::startCapture(IRxTxDevice *device)
{
    if (device == nullptr)
    {
        return false;
    }
    // Stop any previous session.
    stopCapture();

    if (!device->startReceive())
    {
        return false;
    }

    rawRing_.reset();
    workerBufLen_ = 0U;
    activeDevice_ = device;
    return true;
}

void RadioManager::stopCapture()
{
    if (activeDevice_ != nullptr)
    {
        activeDevice_->stop();
        activeDevice_ = nullptr;
    }
}

bool RadioManager::isCapturing() const
{
    return activeDevice_ != nullptr;
}

// ── Replay / Transmit ────────────────────────────────────────────────────────

bool RadioManager::transmit(const SignalRecord &record)
{
    // Find the protocol to encode the signal.
    RadioProtocol *proto = ProtocolRegistry::findByName(record.protocolName);
    if (proto == nullptr)
    {
        return false;
    }

    // Find a device with matching modulation.
    IRxTxDevice *dev = nullptr;
    for (size_t i = 0U; i < deviceCount_; ++i)
    {
        if (devices_[i]->modulation() == record.modulation)
        {
            dev = devices_[i];
            break;
        }
    }
    if (dev == nullptr)
    {
        return false;
    }

    // Encode the signal into raw timings.
    const size_t len = proto->encode(record, txBuf_, MAX_RAW_SAMPLES);
    if (len == 0U)
    {
        return false;
    }

    if (!dev->startTransmit())
    {
        return false;
    }

    const bool ok = dev->write(reinterpret_cast<const uint8_t *>(txBuf_),
                               len * sizeof(int32_t));
    dev->stop();
    return ok;
}

// ── Last captured signal ─────────────────────────────────────────────────────

bool RadioManager::hasLastRecord() const
{
    return hasLast_;
}

const SignalRecord &RadioManager::lastRecord() const
{
    return lastRecord_;
}

// ── Worker task ──────────────────────────────────────────────────────────────

void RadioManager::workerEntry(void * /*param*/)
{
    RadioManager &mgr = instance();
    for (;;)
    {
        mgr.processBuffer();
        // Yield to other tasks for a short period.
        // In a real build this would be vTaskDelay(pdMS_TO_TICKS(5U)).
    }
}

void RadioManager::processBuffer()
{
    if (activeDevice_ == nullptr)
    {
        return;
    }

    // Poll device for new data and push into ring buffer.
    const size_t got = activeDevice_->read(readBuf_, sizeof(readBuf_));

    // Interpret the read bytes as int32_t timing samples when enough data
    // is available (device adapters are expected to deliver aligned data).
    const size_t samples = got / sizeof(int32_t);
    const int32_t *samplePtr = reinterpret_cast<const int32_t *>(readBuf_);
    for (size_t i = 0U; i < samples; ++i)
    {
        rawRing_.push(samplePtr[i]);
    }

    // Drain the ring buffer into the worker scratch buffer.
    int32_t sample;
    while (rawRing_.pop(sample))
    {
        if (workerBufLen_ < MAX_RAW_SAMPLES)
        {
            workerBuf_[workerBufLen_++] = sample;
        }
        else
        {
            // Buffer full – attempt decode with what we have.
            break;
        }
    }

    if (workerBufLen_ == 0U)
    {
        return;
    }

    // Try to decode the accumulated samples.
    SignalRecord record;
    record.clear();
    RadioProtocol *matched = ProtocolRegistry::tryDecodeAll(
        workerBuf_, workerBufLen_, record);

    if (matched != nullptr)
    {
        lastRecord_ = record;
        hasLast_ = true;
        workerBufLen_ = 0U; // Consume the buffer on successful decode.
    }
}

} // namespace hackos::radio
