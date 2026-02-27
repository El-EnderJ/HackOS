#include "hardware/radio/protocol_nec.h"

namespace hackos::radio {

bool Protocol_NEC::matchesDuration(int32_t measured, int32_t nominal)
{
    const int32_t absMeasured = (measured < 0) ? -measured : measured;
    const int32_t absNominal = (nominal < 0) ? -nominal : nominal;
    const int32_t margin = absNominal * TOLERANCE_PCT / 100;
    const int32_t diff = absMeasured - absNominal;
    return (diff < 0 ? -diff : diff) <= margin;
}

bool Protocol_NEC::tryDecode(const int32_t *rawTimings,
                             size_t count,
                             SignalRecord &record)
{
    if (rawTimings == nullptr || count < 4U)
    {
        return false;
    }

    // Expect leader: mark ≈ 9000 µs, space ≈ 4500 µs.
    size_t idx = 0U;

    // Find leader mark.
    bool leaderFound = false;
    for (; idx + 1U < count; ++idx)
    {
        const int32_t mark = (rawTimings[idx] < 0) ? -rawTimings[idx] : rawTimings[idx];
        const int32_t space = (rawTimings[idx + 1U] < 0) ? -rawTimings[idx + 1U] : rawTimings[idx + 1U];
        if (matchesDuration(mark, LEADER_MARK_US) && matchesDuration(space, LEADER_SPACE_US))
        {
            idx += 2U;
            leaderFound = true;
            break;
        }
    }
    if (!leaderFound)
    {
        return false;
    }

    // Decode 32 data bits (bit-mark + bit-space pairs).
    uint64_t decoded = 0ULL;
    uint16_t bits = 0U;
    for (; idx + 1U < count && bits < NEC_BITS; idx += 2U)
    {
        const int32_t mark = (rawTimings[idx] < 0) ? -rawTimings[idx] : rawTimings[idx];
        const int32_t space = (rawTimings[idx + 1U] < 0) ? -rawTimings[idx + 1U] : rawTimings[idx + 1U];

        if (!matchesDuration(mark, BIT_MARK_US))
        {
            break;
        }

        if (matchesDuration(space, ONE_SPACE_US))
        {
            decoded = (decoded << 1U) | 1ULL;
        }
        else if (matchesDuration(space, ZERO_SPACE_US))
        {
            decoded = (decoded << 1U);
        }
        else
        {
            break;
        }
        ++bits;
    }

    if (bits != NEC_BITS)
    {
        return false;
    }

    record.clear();
    record.frequencyHz = CARRIER_HZ;
    record.modulation = Modulation::IR_PULSE;
    copyProtocolName(record.protocolName, "NEC");
    record.bitCount = NEC_BITS;
    record.decodedValue = decoded;

    const size_t toCopy = (count > MAX_RAW_SAMPLES) ? MAX_RAW_SAMPLES : count;
    for (size_t i = 0U; i < toCopy; ++i)
    {
        record.rawTimings[i] = rawTimings[i];
    }
    record.rawCount = static_cast<uint16_t>(toCopy);

    return true;
}

size_t Protocol_NEC::encode(const SignalRecord &record,
                            int32_t *timings,
                            size_t maxTimings)
{
    if (timings == nullptr || record.bitCount != NEC_BITS)
    {
        return 0U;
    }

    size_t idx = 0U;

    // Leader: mark + space.
    if (idx + 2U > maxTimings) { return 0U; }
    timings[idx++] = LEADER_MARK_US;
    timings[idx++] = -LEADER_SPACE_US;

    // 32 data bits.
    for (uint16_t b = 0U; b < NEC_BITS; ++b)
    {
        if (idx + 2U > maxTimings) { break; }
        timings[idx++] = BIT_MARK_US;
        const bool bit = ((record.decodedValue >> (NEC_BITS - 1U - b)) & 1ULL) != 0U;
        timings[idx++] = bit ? -ONE_SPACE_US : -ZERO_SPACE_US;
    }

    // Stop bit.
    if (idx < maxTimings)
    {
        timings[idx++] = BIT_MARK_US;
    }

    return idx;
}

} // namespace hackos::radio
