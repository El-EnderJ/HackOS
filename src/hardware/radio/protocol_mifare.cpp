#include "hardware/radio/protocol_mifare.h"

namespace hackos::radio {

bool Protocol_Mifare::tryDecode(const int32_t *rawTimings,
                                size_t count,
                                SignalRecord &record)
{
    // Mifare data arrives as a byte stream with the UID at the front.
    // Minimum: 4-byte UID.
    if (rawTimings == nullptr || count < UID_LEN_4)
    {
        return false;
    }

    // Determine UID length (4 or 7 bytes).
    size_t uidLen = UID_LEN_4;
    if (count >= UID_LEN_7 + BYTES_PER_BLOCK)
    {
        uidLen = UID_LEN_7;
    }

    // Build a 32-bit or 56-bit UID value.
    uint64_t uidValue = 0ULL;
    for (size_t i = 0U; i < uidLen; ++i)
    {
        uidValue = (uidValue << 8U) | (static_cast<uint64_t>(rawTimings[i]) & 0xFFU);
    }

    record.clear();
    record.frequencyHz = CARRIER_HZ;
    record.modulation = Modulation::NFC_A;
    copyProtocolName(record.protocolName, "MIFARE");
    record.bitCount = static_cast<uint16_t>(uidLen * 8U);
    record.decodedValue = uidValue;

    // Store full byte dump in rawTimings.
    const size_t toCopy = (count > MAX_RAW_SAMPLES) ? MAX_RAW_SAMPLES : count;
    for (size_t i = 0U; i < toCopy; ++i)
    {
        record.rawTimings[i] = rawTimings[i];
    }
    record.rawCount = static_cast<uint16_t>(toCopy);

    return true;
}

size_t Protocol_Mifare::encode(const SignalRecord &record,
                               int32_t *timings,
                               size_t maxTimings)
{
    if (timings == nullptr || record.bitCount == 0U)
    {
        return 0U;
    }

    // Re-emit the UID bytes.
    const size_t uidLen = record.bitCount / 8U;
    if (uidLen > maxTimings)
    {
        return 0U;
    }

    for (size_t i = 0U; i < uidLen; ++i)
    {
        timings[i] = static_cast<int32_t>(
            (record.decodedValue >> ((uidLen - 1U - i) * 8U)) & 0xFFU);
    }

    // Append stored block data if available.
    size_t written = uidLen;
    if (record.rawCount > uidLen)
    {
        const size_t extra = record.rawCount - uidLen;
        const size_t room = maxTimings - written;
        const size_t toCopy = (extra < room) ? extra : room;
        for (size_t i = 0U; i < toCopy; ++i)
        {
            timings[written + i] = record.rawTimings[uidLen + i];
        }
        written += toCopy;
    }

    return written;
}

} // namespace hackos::radio
