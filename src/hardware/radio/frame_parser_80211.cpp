/**
 * @file frame_parser_80211.cpp
 * @brief IEEE 802.11 frame parser and builder implementation.
 */

#include "hardware/radio/frame_parser_80211.h"

#include <cstring>

namespace hackos::radio {

// ── Classification ───────────────────────────────────────────────────────────

bool isMgmtFrame(const uint8_t *payload, size_t len)
{
    if (payload == nullptr || len < MGMT_HDR_LEN)
    {
        return false;
    }
    return (payload[0] & FC_TYPE_MASK) == FC_TYPE_MGMT;
}

bool isDataFrame(const uint8_t *payload, size_t len)
{
    if (payload == nullptr || len < MGMT_HDR_LEN)
    {
        return false;
    }
    return (payload[0] & FC_TYPE_MASK) == FC_TYPE_DATA;
}

// ── Tagged-parameter walker ──────────────────────────────────────────────────

/**
 * @brief Walk tagged parameters and extract SSID (tag 0) and channel (tag 3).
 *
 * @param tags    Pointer to the start of the tagged-parameter region.
 * @param tagsLen Length of the tagged-parameter region.
 * @param[out] ssid    Destination for SSID string (33 bytes).
 * @param[out] channel Destination for DS Parameter Set channel.
 */
static void extractTags(const uint8_t *tags, size_t tagsLen,
                        char *ssid, uint8_t *channel)
{
    size_t offset = 0U;

    while (offset + 2U <= tagsLen)
    {
        const uint8_t tagId  = tags[offset];
        const uint8_t tagLen = tags[offset + 1U];

        if (offset + 2U + tagLen > tagsLen)
        {
            break; // malformed tag – stop walking
        }

        if (tagId == 0U) // SSID
        {
            const uint8_t copyLen = tagLen > 32U ? 32U : tagLen;
            std::memcpy(ssid, &tags[offset + 2U], copyLen);
            ssid[copyLen] = '\0';
        }
        else if (tagId == 3U && tagLen >= 1U) // DS Parameter Set
        {
            *channel = tags[offset + 2U];
        }

        offset += 2U + tagLen;
    }
}

// ── Management frame parser ──────────────────────────────────────────────────

MgmtFrameInfo parseMgmtFrame(const uint8_t *payload, size_t len, int8_t rssi)
{
    MgmtFrameInfo info{};
    info.valid   = false;
    info.rssi    = rssi;
    info.channel = 0U;
    info.ssid[0] = '\0';

    if (!isMgmtFrame(payload, len))
    {
        return info;
    }

    info.subtype = payload[0] & FC_SUBTYPE_MASK;

    // Addresses
    std::memcpy(info.addr1, &payload[4],  6U);
    std::memcpy(info.addr2, &payload[10], 6U);
    std::memcpy(info.addr3, &payload[16], 6U);

    // For Beacons and Probe Responses the tagged params start at offset 36
    // (24-byte header + 12-byte fixed parameters: timestamp 8 + interval 2 +
    // capabilities 2).
    // For Probe Requests the tagged params start at offset 24 (no fixed params).
    size_t tagOffset = MGMT_HDR_LEN;

    if (info.subtype == SUBTYPE_BEACON || info.subtype == SUBTYPE_PROBE_RESP)
    {
        tagOffset = MGMT_HDR_LEN + 12U; // skip fixed parameters
    }
    // Probe Request has no fixed parameters, tags start right after header.

    if (tagOffset < len)
    {
        extractTags(&payload[tagOffset], len - tagOffset,
                    info.ssid, &info.channel);
    }

    info.valid = true;
    return info;
}

// ── EAPOL handshake detection ────────────────────────────────────────────────

bool isEapolHandshake(const uint8_t *payload, size_t len)
{
    if (payload == nullptr || len < MGMT_HDR_LEN)
    {
        return false;
    }

    // Must be a data frame
    if ((payload[0] & FC_TYPE_MASK) != FC_TYPE_DATA)
    {
        return false;
    }

    // Determine header length: QoS data subtype has 2 extra bytes.
    // QoS Data subtype = 0x88 in FC byte 0 (type=Data, subtype=1000).
    const uint8_t subtype = payload[0] & FC_SUBTYPE_MASK;
    size_t hdrLen = MGMT_HDR_LEN;
    if (subtype == 0x80U) // QoS data
    {
        hdrLen = 26U;
    }

    // LLC/SNAP header: AA AA 03 00 00 00 88 8E
    if (hdrLen + 8U > len)
    {
        return false;
    }

    static constexpr uint8_t LLC_SNAP_EAPOL[] = {
        0xAAU, 0xAAU, 0x03U, 0x00U, 0x00U, 0x00U, 0x88U, 0x8EU
    };

    if (std::memcmp(&payload[hdrLen], LLC_SNAP_EAPOL, 8U) != 0)
    {
        return false;
    }

    // EAPOL header starts after LLC/SNAP (8 bytes):
    //   Version (1) + Type (1) + Length (2) + Descriptor Type (1) + Key Info (2)
    // Key Info is at offset hdrLen + 8 + 5 = hdrLen + 13
    const size_t keyInfoOffset = hdrLen + 13U;
    if (keyInfoOffset + 2U > len)
    {
        return false;
    }

    // EAPOL type should be 3 (Key)
    if (payload[hdrLen + 9U] != 0x03U)
    {
        return false;
    }

    // Key Info (big-endian 16-bit)
    const uint16_t keyInfo =
        (static_cast<uint16_t>(payload[keyInfoOffset]) << 8U) |
        static_cast<uint16_t>(payload[keyInfoOffset + 1U]);

    // Pairwise bit (bit 3, mask 0x0008)
    if ((keyInfo & 0x0008U) == 0U)
    {
        return false; // Not a pairwise key exchange
    }

    // Message 1/4: Ack=1 (bit 7, mask 0x0080), MIC=0 (bit 8, mask 0x0100)
    // Message 2/4: Ack=0, MIC=1
    const bool ack = (keyInfo & 0x0080U) != 0U;
    const bool mic = (keyInfo & 0x0100U) != 0U;

    // Accept message 1/4 (ack && !mic) or message 2/4 (!ack && mic)
    return (ack && !mic) || (!ack && mic);
}

// ── Deauth frame builder ─────────────────────────────────────────────────────

size_t buildDeauthFrame(uint8_t *frame, const uint8_t *ap_mac,
                        const uint8_t *client_mac, uint16_t reason)
{
    if (frame == nullptr || ap_mac == nullptr || client_mac == nullptr)
    {
        return 0U;
    }

    std::memset(frame, 0, DEAUTH_FRAME_LEN);

    // Frame Control: Management, subtype Deauth (0xC0)
    frame[0] = 0xC0U;
    frame[1] = 0x00U;

    // Duration
    frame[2] = 0x00U;
    frame[3] = 0x00U;

    // Address 1 – DA (client or broadcast)
    std::memcpy(&frame[4], client_mac, 6U);

    // Address 2 – SA (AP BSSID, so the client accepts the frame)
    std::memcpy(&frame[10], ap_mac, 6U);

    // Address 3 – BSSID
    std::memcpy(&frame[16], ap_mac, 6U);

    // Sequence Control
    frame[22] = 0x00U;
    frame[23] = 0x00U;

    // Reason Code (little-endian)
    frame[24] = static_cast<uint8_t>(reason & 0xFFU);
    frame[25] = static_cast<uint8_t>((reason >> 8U) & 0xFFU);

    return DEAUTH_FRAME_LEN;
}

// ── Beacon frame builder ─────────────────────────────────────────────────────

size_t buildBeaconFrame(uint8_t *frame, size_t maxLen, const char *ssid,
                        const uint8_t *src_mac, uint8_t channel, uint16_t seq)
{
    if (frame == nullptr || ssid == nullptr || src_mac == nullptr)
    {
        return 0U;
    }

    const size_t ssidLen = std::strlen(ssid);
    const size_t clampedSsidLen = ssidLen > 32U ? 32U : ssidLen;

    // Header (24) + Fixed params (12) + SSID tag (2+ssid) +
    // Supported Rates (2+8) + DS Param (2+1) = 51 + ssidLen
    const size_t totalLen = 24U + 12U + 2U + clampedSsidLen + 10U + 3U;
    if (totalLen > maxLen)
    {
        return 0U;
    }

    std::memset(frame, 0, totalLen);
    size_t pos = 0U;

    // ── MAC header (24 bytes) ────────────────────────────────────────────
    frame[pos++] = 0x80U; // Frame Control: Management, Beacon
    frame[pos++] = 0x00U;
    frame[pos++] = 0x00U; // Duration
    frame[pos++] = 0x00U;

    // DA: broadcast
    std::memset(&frame[pos], 0xFFU, 6U);
    pos += 6U;

    // SA: source MAC
    std::memcpy(&frame[pos], src_mac, 6U);
    pos += 6U;

    // BSSID: same as source
    std::memcpy(&frame[pos], src_mac, 6U);
    pos += 6U;

    // Sequence Control (12-bit seq number in upper 12 bits)
    frame[pos++] = static_cast<uint8_t>((seq & 0x0FU) << 4U);
    frame[pos++] = static_cast<uint8_t>((seq >> 4U) & 0xFFU);

    // ── Fixed parameters (12 bytes) ──────────────────────────────────────
    // Timestamp: 8 bytes (left as 0 – the hardware fills it)
    pos += 8U;

    // Beacon Interval: 100 TU (~102.4 ms)
    frame[pos++] = 0x64U;
    frame[pos++] = 0x00U;

    // Capability Information: ESS + Privacy
    frame[pos++] = 0x31U;
    frame[pos++] = 0x04U;

    // ── Tagged parameters ────────────────────────────────────────────────

    // SSID (Tag 0)
    frame[pos++] = 0x00U;
    frame[pos++] = static_cast<uint8_t>(clampedSsidLen);
    std::memcpy(&frame[pos], ssid, clampedSsidLen);
    pos += clampedSsidLen;

    // Supported Rates (Tag 1) – 802.11b/g mandatory rates
    frame[pos++] = 0x01U;
    frame[pos++] = 0x08U;
    frame[pos++] = 0x82U; // 1  Mbps (basic)
    frame[pos++] = 0x84U; // 2  Mbps (basic)
    frame[pos++] = 0x8BU; // 5.5 Mbps (basic)
    frame[pos++] = 0x96U; // 11 Mbps (basic)
    frame[pos++] = 0x24U; // 18 Mbps
    frame[pos++] = 0x30U; // 24 Mbps
    frame[pos++] = 0x48U; // 36 Mbps
    frame[pos++] = 0x6CU; // 54 Mbps

    // DS Parameter Set (Tag 3)
    frame[pos++] = 0x03U;
    frame[pos++] = 0x01U;
    frame[pos++] = channel;

    return pos;
}

} // namespace hackos::radio
