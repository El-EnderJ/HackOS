#include "hardware/radio/protocol_ook.h"

#include <cstdlib>
#include <cstring>

namespace hackos::radio {

bool Protocol_OOK::matchesDuration(int32_t measured, int32_t nominal)
{
    const int32_t absMeasured = (measured < 0) ? -measured : measured;
    const int32_t absNominal = (nominal < 0) ? -nominal : nominal;
    const int32_t margin = absNominal * TOLERANCE_PCT / 100;
    const int32_t diff = absMeasured - absNominal;
    return (diff < 0 ? -diff : diff) <= margin;
}

bool Protocol_OOK::tryDecode(const int32_t *rawTimings,
                             size_t count,
                             SignalRecord &record)
{
    if (rawTimings == nullptr || count < 4U)
    {
        return false;
    }

    // Locate the sync preamble (a gap longer than SYNC_MIN_US).
    size_t dataStart = 0U;
    bool syncFound = false;
    for (size_t i = 0U; i < count; ++i)
    {
        const int32_t abs = (rawTimings[i] < 0) ? -rawTimings[i] : rawTimings[i];
        if (abs >= SYNC_MIN_US)
        {
            dataStart = i + 1U;
            syncFound = true;
            break;
        }
    }
    if (!syncFound || dataStart >= count)
    {
        return false;
    }

    // Decode bit pairs: short-mark + long-gap = 0, long-mark + short-gap = 1.
    uint64_t decoded = 0ULL;
    uint16_t bits = 0U;
    for (size_t i = dataStart; i + 1U < count; i += 2U)
    {
        const int32_t mark = (rawTimings[i] < 0) ? -rawTimings[i] : rawTimings[i];
        const int32_t space = (rawTimings[i + 1U] < 0) ? -rawTimings[i + 1U] : rawTimings[i + 1U];

        if (matchesDuration(mark, SHORT_PULSE_US) && matchesDuration(space, LONG_PULSE_US))
        {
            decoded = (decoded << 1U);
            ++bits;
        }
        else if (matchesDuration(mark, LONG_PULSE_US) && matchesDuration(space, SHORT_PULSE_US))
        {
            decoded = (decoded << 1U) | 1ULL;
            ++bits;
        }
        else
        {
            break; // End of valid bit stream.
        }
    }

    if (bits == 0U)
    {
        return false;
    }

    // Fill the record.
    record.clear();
    record.frequencyHz = DEFAULT_FREQ_HZ;
    record.modulation = Modulation::OOK;
    std::strncpy(record.protocolName, "OOK", MAX_PROTOCOL_NAME - 1U);
    record.protocolName[MAX_PROTOCOL_NAME - 1U] = '\0';
    record.bitCount = bits;
    record.decodedValue = decoded;

    // Copy raw timings.
    const size_t toCopy = (count > MAX_RAW_SAMPLES) ? MAX_RAW_SAMPLES : count;
    for (size_t i = 0U; i < toCopy; ++i)
    {
        record.rawTimings[i] = rawTimings[i];
    }
    record.rawCount = static_cast<uint16_t>(toCopy);

    return true;
}

size_t Protocol_OOK::encode(const SignalRecord &record,
                            int32_t *timings,
                            size_t maxTimings)
{
    if (timings == nullptr || maxTimings < 2U || record.bitCount == 0U)
    {
        return 0U;
    }

    size_t idx = 0U;

    // Emit sync preamble: short mark + long gap.
    if (idx + 2U > maxTimings) { return 0U; }
    timings[idx++] = SHORT_PULSE_US;
    timings[idx++] = -(SYNC_MIN_US + LONG_PULSE_US);

    // Emit data bits.
    for (uint16_t b = 0U; b < record.bitCount; ++b)
    {
        if (idx + 2U > maxTimings) { break; }
        const bool bit = ((record.decodedValue >> (record.bitCount - 1U - b)) & 1ULL) != 0U;
        if (bit)
        {
            timings[idx++] = LONG_PULSE_US;
            timings[idx++] = -SHORT_PULSE_US;
        }
        else
        {
            timings[idx++] = SHORT_PULSE_US;
            timings[idx++] = -LONG_PULSE_US;
        }
    }

    return idx;
}

} // namespace hackos::radio
