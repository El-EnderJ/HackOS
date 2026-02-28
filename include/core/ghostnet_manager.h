/**
 * @file ghostnet_manager.h
 * @brief GhostNet – ESP-NOW mesh communication manager for HackOS.
 *
 * Implements a zero-configuration mesh network using ESP-NOW that allows
 * multiple HackOS devices to discover each other, share captured data
 * (WiFi handshakes, NFC UIDs, etc.), exchange chat messages, and
 * coordinate remote execution of distributed attacks.
 *
 * Features:
 *  - **Auto-discovery**: Periodic broadcast beacons; any HackOS device
 *    within ~200 m range is detected automatically.
 *  - **Encrypted sync**: Shared captures are transmitted with a PMK/LMK
 *    encryption layer provided by ESP-NOW.
 *  - **Remote execution**: A "Master" node can instruct peer "Nodes" to
 *    execute BLE spam or WiFi deauth simultaneously.
 *  - **Chat**: Simple text messaging between peers.
 *
 * @warning **Legal notice**: Coordinated wireless attacks against
 * networks or devices you do not own or have explicit written
 * authorisation to test is illegal in most jurisdictions.
 */

#pragma once

#include <cstdint>
#include <cstddef>

namespace hackos::core {

// ── Message types ────────────────────────────────────────────────────────────

enum class GhostMsgType : uint8_t
{
    BEACON      = 0x01, ///< Discovery beacon (broadcast)
    BEACON_ACK  = 0x02, ///< Acknowledgement of a beacon
    CHAT        = 0x10, ///< Free-form text message
    SYNC_DATA   = 0x20, ///< Captured data synchronisation payload
    CMD_REQUEST = 0x30, ///< Remote execution command from Master
    CMD_ACK     = 0x31, ///< Node acknowledges command receipt
};

// ── Remote command identifiers ───────────────────────────────────────────────

enum class GhostCmd : uint8_t
{
    NONE        = 0x00,
    BLE_SPAM    = 0x01, ///< Start BLE advertising spam
    WIFI_DEAUTH = 0x02, ///< Start WiFi deauth burst
    STOP        = 0xFF, ///< Stop any running remote task
};

// ── Peer information ─────────────────────────────────────────────────────────

struct GhostPeer
{
    uint8_t mac[6];         ///< MAC address of the peer
    int8_t  rssi;           ///< Last known RSSI (dBm)
    uint32_t lastSeenMs;    ///< millis() timestamp of last message
    char    name[16];       ///< Human-readable node name
    bool    active;         ///< Slot in use?
};

// ── Wire-format header (fits within ESP-NOW 250-byte limit) ──────────────────

struct __attribute__((packed)) GhostPacket
{
    uint8_t      magic[2];   ///< {'G','N'} – GhostNet identifier
    GhostMsgType type;       ///< Message type
    uint8_t      seqNo;      ///< Rolling sequence number
    uint8_t      srcMac[6];  ///< Sender MAC (redundant but avoids API lookup)
    char         srcName[16];///< Sender display name
    uint8_t      payloadLen; ///< Length of payload following this header
    uint8_t      payload[0]; ///< Variable-length payload (max ~220 bytes)
};

static constexpr size_t GHOST_HEADER_SIZE = sizeof(GhostPacket);
static constexpr size_t GHOST_MAX_PAYLOAD = 250U - GHOST_HEADER_SIZE;
static constexpr uint8_t GHOST_MAGIC[2] = {'G', 'N'};

// ── Chat message (stored in ring buffer) ─────────────────────────────────────

struct GhostChatMsg
{
    char sender[16];
    char text[64];
    uint32_t timestampMs;
};

// ── GhostNetManager singleton ────────────────────────────────────────────────

class GhostNetManager
{
public:
    static GhostNetManager &instance();

    /// @brief Initialise ESP-NOW, set encryption keys, start beacon task.
    bool init();

    /// @brief Shut down ESP-NOW and clean up peers.
    void deinit();

    /// @brief Call periodically from the main loop (~10 ms) to age-out
    ///        stale peers and re-broadcast beacons.
    void tick();

    // ── Peer accessors ───────────────────────────────────────────────────

    static constexpr size_t MAX_PEERS = 8U;

    size_t peerCount() const;
    const GhostPeer *peer(size_t index) const;

    // ── Chat ─────────────────────────────────────────────────────────────

    static constexpr size_t CHAT_RING_SIZE = 16U;

    /// @brief Send a chat message to all peers.
    bool sendChat(const char *text);

    /// @brief Number of chat messages in the ring buffer.
    size_t chatCount() const { return chatCount_; }

    /// @brief Access a chat message (0 = oldest in ring).
    const GhostChatMsg *chatAt(size_t index) const;

    // ── Data sync ────────────────────────────────────────────────────────

    /// @brief Broadcast a captured data blob (type string + payload).
    bool syncData(const char *dataType, const uint8_t *data, size_t len);

    // ── Remote execution (Master) ────────────────────────────────────────

    /// @brief Send a command to all peers.
    bool sendCommand(GhostCmd cmd);

    /// @brief Last command received from a Master (for Node mode).
    GhostCmd lastReceivedCmd() const { return lastCmd_; }

    /// @brief Clear the last received command.
    void clearLastCmd() { lastCmd_ = GhostCmd::NONE; }

    // ── Node name ────────────────────────────────────────────────────────

    const char *nodeName() const { return nodeName_; }

    /// @brief Whether ESP-NOW is active.
    bool isActive() const { return initialized_; }

private:
    GhostNetManager();

    /// @brief Build and send a packet to all peers (or broadcast).
    bool sendPacket(GhostMsgType type, const uint8_t *payload, size_t len);

    /// @brief Register a discovered peer with ESP-NOW.
    bool addOrUpdatePeer(const uint8_t *mac, int8_t rssi, const char *name);

    /// @brief Remove peers that haven't been heard from recently.
    void pruneStale();

    /// @brief Send a discovery beacon (broadcast).
    void sendBeacon();

    /// @brief Process a received GhostNet packet.
    void handlePacket(const uint8_t *mac, const uint8_t *data, int len,
                      int8_t rssi);

    // ── ESP-NOW callbacks (static, forwarded to singleton) ───────────────

    static void onDataRecv(const uint8_t *mac, const uint8_t *data, int len);
    static void onDataSent(const uint8_t *mac, bool success);

    // ── State ────────────────────────────────────────────────────────────

    bool initialized_;
    uint8_t seqNo_;
    char nodeName_[16];
    uint8_t ownMac_[6];

    GhostPeer peers_[MAX_PEERS];

    GhostChatMsg chatRing_[CHAT_RING_SIZE];
    size_t chatHead_;   ///< Next write position
    size_t chatCount_;  ///< Messages in ring

    GhostCmd lastCmd_;

    uint32_t lastBeaconMs_;
    static constexpr uint32_t BEACON_INTERVAL_MS   = 5000U;  ///< 5 s
    static constexpr uint32_t PEER_TIMEOUT_MS      = 30000U; ///< 30 s
};

} // namespace hackos::core
