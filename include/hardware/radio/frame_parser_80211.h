/**
 * @file frame_parser_80211.h
 * @brief IEEE 802.11 frame parser and builder for offensive WiFi operations.
 *
 * Provides:
 *  - Management frame parsing (Beacon, Probe Request/Response) to extract
 *    BSSIDs, SSIDs, and channel information.
 *  - EAPOL handshake detection (frames 1/4 and 2/4) in data frames.
 *  - Raw deauthentication frame builder for targeted or broadcast injection.
 *  - Raw beacon frame builder for fake-AP / beacon-spam operations.
 *
 * All functions are stateless and operate on raw byte buffers, suitable for
 * use inside the esp_wifi promiscuous-mode callback or the Radio_Task.
 *
 * @warning **Legal notice**: Injecting 802.11 frames on networks you do not
 * own or have explicit written authorisation to test is illegal in most
 * jurisdictions.  Use exclusively in authorised environments.
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace hackos::radio {

// ── 802.11 Frame-type constants ──────────────────────────────────────────────

/// Frame Control field: type mask (bits 2-3 of byte 0).
static constexpr uint8_t FC_TYPE_MASK    = 0x0CU;
static constexpr uint8_t FC_TYPE_MGMT    = 0x00U;
static constexpr uint8_t FC_TYPE_DATA    = 0x08U;

/// Frame Control field: subtype mask (bits 4-7 of byte 0).
static constexpr uint8_t FC_SUBTYPE_MASK = 0xF0U;

/// Management-frame subtypes (value of FC byte 0 & 0xF0).
static constexpr uint8_t SUBTYPE_PROBE_REQ  = 0x40U;
static constexpr uint8_t SUBTYPE_PROBE_RESP = 0x50U;
static constexpr uint8_t SUBTYPE_BEACON     = 0x80U;
static constexpr uint8_t SUBTYPE_DEAUTH     = 0xC0U;

/// Minimum management frame header length (bytes).
static constexpr size_t MGMT_HDR_LEN = 24U;

/// Minimum deauthentication frame length (header + reason).
static constexpr size_t DEAUTH_FRAME_LEN = 26U;

// ── Parsed frame information ─────────────────────────────────────────────────

/**
 * @brief Result of parsing a management frame.
 */
struct MgmtFrameInfo
{
    uint8_t subtype;      ///< FC subtype nibble (e.g. 0x80 = Beacon)
    uint8_t addr1[6];     ///< Address 1 – Destination / Receiver
    uint8_t addr2[6];     ///< Address 2 – Source / Transmitter
    uint8_t addr3[6];     ///< Address 3 – BSSID
    char    ssid[33];     ///< Extracted SSID (NUL-terminated, may be empty)
    int8_t  rssi;         ///< Signal strength (from RX metadata)
    uint8_t channel;      ///< Channel (from DS Parameter Set tag, or 0)
    bool    valid;        ///< True if the frame was parsed successfully
};

// ── Classification helpers ───────────────────────────────────────────────────

/**
 * @brief Check whether a raw 802.11 frame is a management frame.
 * @param payload  Pointer to the first byte of the MAC header.
 * @param len      Total payload length in bytes.
 * @return true if the frame type field indicates Management.
 */
bool isMgmtFrame(const uint8_t *payload, size_t len);

/**
 * @brief Check whether a raw 802.11 frame is a data frame.
 */
bool isDataFrame(const uint8_t *payload, size_t len);

// ── Management frame parser ──────────────────────────────────────────────────

/**
 * @brief Parse a management frame and extract BSSID, SSID, and channel.
 *
 * Works for Beacons, Probe Requests, and Probe Responses.  Tagged
 * parameters are walked to find the SSID element (tag 0) and DS
 * Parameter Set (tag 3).
 *
 * @param payload  Raw 802.11 frame bytes.
 * @param len      Length of @p payload.
 * @param rssi     Signal strength from the RX control metadata.
 * @return A MgmtFrameInfo structure; check `.valid` before using.
 */
MgmtFrameInfo parseMgmtFrame(const uint8_t *payload, size_t len, int8_t rssi);

// ── EAPOL handshake detection ────────────────────────────────────────────────

/**
 * @brief Detect whether a data frame contains an EAPOL handshake message
 *        (frame 1/4 or 2/4 of the 4-way handshake).
 *
 * The function looks for the LLC/SNAP header (AA AA 03 00 00 00 88 8E)
 * following the 802.11 data header, then inspects the EAPOL-Key Info
 * field for the Pairwise + Ack/MIC bit patterns.
 *
 * @param payload  Raw 802.11 frame bytes.
 * @param len      Length of @p payload.
 * @return true if the frame carries an EAPOL handshake message 1/4 or 2/4.
 */
bool isEapolHandshake(const uint8_t *payload, size_t len);

// ── Frame builders ───────────────────────────────────────────────────────────

/**
 * @brief Build a raw IEEE 802.11 deauthentication frame.
 *
 * @param[out] frame       Output buffer (must be >= DEAUTH_FRAME_LEN bytes).
 * @param      ap_mac      6-byte BSSID of the target AP.
 * @param      client_mac  6-byte client MAC, or broadcast (FF:FF:FF:FF:FF:FF).
 * @param      reason      Reason code (default 7 – class-3 frame from
 *                         non-associated station).
 * @return DEAUTH_FRAME_LEN (26) on success, 0 on null-pointer input.
 */
size_t buildDeauthFrame(uint8_t *frame, const uint8_t *ap_mac,
                        const uint8_t *client_mac, uint16_t reason = 7U);

/**
 * @brief Build a raw IEEE 802.11 beacon frame for fake-AP operations.
 *
 * The generated frame includes:
 *  - Management header (type Beacon, broadcast DA)
 *  - Fixed parameters (timestamp, beacon interval, capabilities)
 *  - Tagged parameters: SSID, Supported Rates, DS Parameter Set
 *
 * @param[out] frame    Output buffer.
 * @param      maxLen   Size of @p frame in bytes (recommend >= 128).
 * @param      ssid     Null-terminated SSID string (max 32 chars).
 * @param      src_mac  6-byte source/BSSID MAC for the fake AP.
 * @param      channel  Channel number to advertise.
 * @param      seq      Sequence number (incremented per frame by caller).
 * @return Actual frame length written, or 0 on error.
 */
size_t buildBeaconFrame(uint8_t *frame, size_t maxLen, const char *ssid,
                        const uint8_t *src_mac, uint8_t channel, uint16_t seq);

} // namespace hackos::radio
