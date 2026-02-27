#include "hardware/radio/radio_protocol.h"

#include <cstring>

namespace hackos::radio {

RadioProtocol *ProtocolRegistry::protocols_[MAX_PROTOCOLS] = {};
size_t ProtocolRegistry::count_ = 0U;

bool ProtocolRegistry::registerProtocol(RadioProtocol *proto)
{
    if (proto == nullptr || count_ >= MAX_PROTOCOLS)
    {
        return false;
    }
    protocols_[count_++] = proto;
    return true;
}

size_t ProtocolRegistry::count()
{
    return count_;
}

RadioProtocol *ProtocolRegistry::get(size_t index)
{
    return (index < count_) ? protocols_[index] : nullptr;
}

RadioProtocol *ProtocolRegistry::findByName(const char *name)
{
    if (name == nullptr)
    {
        return nullptr;
    }
    for (size_t i = 0U; i < count_; ++i)
    {
        if (std::strcmp(protocols_[i]->name(), name) == 0)
        {
            return protocols_[i];
        }
    }
    return nullptr;
}

RadioProtocol *ProtocolRegistry::tryDecodeAll(const int32_t *rawTimings,
                                              size_t count,
                                              SignalRecord &record)
{
    if (rawTimings == nullptr || count == 0U)
    {
        return nullptr;
    }
    for (size_t i = 0U; i < count_; ++i)
    {
        if (protocols_[i]->tryDecode(rawTimings, count, record))
        {
            return protocols_[i];
        }
    }
    return nullptr;
}

} // namespace hackos::radio
