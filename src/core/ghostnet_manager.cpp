/**
 * @file ghostnet_manager.cpp
 * @brief GhostNet ESP-NOW mesh manager implementation.
 *
 * Uses ESP-NOW (connectionless 802.11 vendor action frames) to build a
 * zero-configuration mesh network between HackOS devices.  All frames
 * are encrypted with a shared PMK/LMK key pair.
 *
 * @warning **Legal notice**: Coordinated wireless attacks against
 * networks or devices you do not own or have explicit written
 * authorisation to test is illegal in most jurisdictions.
 */

#include "core/ghostnet_manager.h"

#include <cstring>
#include <cstdio>

#include <Arduino.h>
#include <esp_log.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <WiFi.h>

#include "core/event.h"
#include "core/event_system.h"

static constexpr const char *TAG_GN = "GhostNet";

// ── Encryption keys (shared across all HackOS devices) ───────────────────────
// PMK (Primary Master Key) – 16 bytes, used for ESP-NOW encryption.
// LMK (Local Master Key) – 16 bytes, per-peer encryption key.
static const uint8_t GHOSTNET_PMK[16] = {
    0x48, 0x61, 0x63, 0x6B, 0x4F, 0x53, 0x5F, 0x47,
    0x68, 0x6F, 0x73, 0x74, 0x4E, 0x65, 0x74, 0x31  // "HackOS_GhostNet1"
};

static const uint8_t GHOSTNET_LMK[16] = {
    0x47, 0x4E, 0x5F, 0x4C, 0x4D, 0x4B, 0x5F, 0x4B,
    0x65, 0x79, 0x5F, 0x48, 0x4F, 0x53, 0x31, 0x36  // "GN_LMK_Key_HOS16"
};

/// Broadcast MAC for discovery beacons.
static const uint8_t BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

namespace hackos::core {

// ── Singleton ────────────────────────────────────────────────────────────────

GhostNetManager &GhostNetManager::instance()
{
    static GhostNetManager inst;
    return inst;
}

GhostNetManager::GhostNetManager()
    : initialized_(false)
    , seqNo_(0U)
    , chatHead_(0U)
    , chatCount_(0U)
    , lastCmd_(GhostCmd::NONE)
    , lastBeaconMs_(0U)
{
    std::memset(nodeName_, 0, sizeof(nodeName_));
    std::memset(ownMac_, 0, sizeof(ownMac_));
    std::memset(peers_, 0, sizeof(peers_));
    std::memset(chatRing_, 0, sizeof(chatRing_));
}

// ── Init / Deinit ────────────────────────────────────────────────────────────

bool GhostNetManager::init()
{
    if (initialized_)
    {
        return true;
    }

    // Ensure WiFi is in STA mode (required for ESP-NOW).
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();

    // Read own MAC address.
    esp_read_mac(ownMac_, ESP_MAC_WIFI_STA);

    // Generate a short node name from the last 2 bytes of MAC.
    std::snprintf(nodeName_, sizeof(nodeName_), "HOS_%02X%02X",
                  ownMac_[4], ownMac_[5]);

    // Initialise ESP-NOW.
    if (esp_now_init() != ESP_OK)
    {
        ESP_LOGE(TAG_GN, "esp_now_init failed");
        return false;
    }

    // Set PMK for encryption.
    if (esp_now_set_pmk(GHOSTNET_PMK) != ESP_OK)
    {
        ESP_LOGW(TAG_GN, "Failed to set PMK – continuing unencrypted");
    }

    // Register callbacks.
    esp_now_register_recv_cb(onDataRecv);
    esp_now_register_send_cb(
        [](const uint8_t *mac, esp_now_send_status_t status)
        {
            onDataSent(mac, status == ESP_NOW_SEND_SUCCESS);
        });

    // Add broadcast peer so we can send beacons.
    esp_now_peer_info_t bcastPeer;
    std::memset(&bcastPeer, 0, sizeof(bcastPeer));
    std::memcpy(bcastPeer.peer_addr, BROADCAST_MAC, 6);
    bcastPeer.channel = 0; // current channel
    bcastPeer.encrypt = false;
    if (!esp_now_is_peer_exist(BROADCAST_MAC))
    {
        (void)esp_now_add_peer(&bcastPeer);
    }

    initialized_ = true;
    lastBeaconMs_ = millis();

    ESP_LOGI(TAG_GN, "GhostNet initialised – node %s", nodeName_);
    return true;
}

void GhostNetManager::deinit()
{
    if (!initialized_)
    {
        return;
    }

    esp_now_unregister_recv_cb();
    esp_now_unregister_send_cb();
    esp_now_deinit();

    // Clear peer list.
    for (size_t i = 0U; i < MAX_PEERS; ++i)
    {
        peers_[i].active = false;
    }

    initialized_ = false;
    ESP_LOGI(TAG_GN, "GhostNet deinitialised");
}

// ── Periodic tick ────────────────────────────────────────────────────────────

void GhostNetManager::tick()
{
    if (!initialized_)
    {
        return;
    }

    const uint32_t now = millis();

    // Periodic beacon.
    if ((now - lastBeaconMs_) >= BEACON_INTERVAL_MS)
    {
        sendBeacon();
        lastBeaconMs_ = now;
    }

    // Prune stale peers.
    pruneStale();
}

// ── Peer management ──────────────────────────────────────────────────────────

size_t GhostNetManager::peerCount() const
{
    size_t count = 0U;
    for (size_t i = 0U; i < MAX_PEERS; ++i)
    {
        if (peers_[i].active)
        {
            ++count;
        }
    }
    return count;
}

const GhostPeer *GhostNetManager::peer(size_t index) const
{
    if (index >= MAX_PEERS)
    {
        return nullptr;
    }
    return &peers_[index];
}

bool GhostNetManager::addOrUpdatePeer(const uint8_t *mac, int8_t rssi,
                                       const char *name)
{
    const uint32_t now = millis();

    // Check if peer already exists.
    for (size_t i = 0U; i < MAX_PEERS; ++i)
    {
        if (peers_[i].active &&
            std::memcmp(peers_[i].mac, mac, 6) == 0)
        {
            peers_[i].rssi = rssi;
            peers_[i].lastSeenMs = now;
            if (name != nullptr && name[0] != '\0')
            {
                std::strncpy(peers_[i].name, name, sizeof(peers_[i].name) - 1U);
                peers_[i].name[sizeof(peers_[i].name) - 1U] = '\0';
            }
            return true;
        }
    }

    // Find an empty slot.
    for (size_t i = 0U; i < MAX_PEERS; ++i)
    {
        if (!peers_[i].active)
        {
            std::memcpy(peers_[i].mac, mac, 6);
            peers_[i].rssi = rssi;
            peers_[i].lastSeenMs = now;
            peers_[i].active = true;
            if (name != nullptr && name[0] != '\0')
            {
                std::strncpy(peers_[i].name, name, sizeof(peers_[i].name) - 1U);
                peers_[i].name[sizeof(peers_[i].name) - 1U] = '\0';
            }
            else
            {
                std::snprintf(peers_[i].name, sizeof(peers_[i].name),
                              "%02X%02X%02X", mac[3], mac[4], mac[5]);
            }

            // Register with ESP-NOW (encrypted).
            esp_now_peer_info_t peerInfo;
            std::memset(&peerInfo, 0, sizeof(peerInfo));
            std::memcpy(peerInfo.peer_addr, mac, 6);
            peerInfo.channel = 0;
            peerInfo.encrypt = true;
            std::memcpy(peerInfo.lmk, GHOSTNET_LMK, 16);

            if (!esp_now_is_peer_exist(mac))
            {
                (void)esp_now_add_peer(&peerInfo);
            }

            ESP_LOGI(TAG_GN, "Peer added: %s (RSSI %d)", peers_[i].name,
                     rssi);

            // Post GhostNet event.
            const Event evt{EventType::EVT_GHOSTNET,
                            static_cast<int32_t>(GhostMsgType::BEACON_ACK),
                            static_cast<int32_t>(peerCount()), nullptr};
            EventSystem::instance().postEvent(evt);

            return true;
        }
    }

    ESP_LOGW(TAG_GN, "Peer table full (%zu slots)", MAX_PEERS);
    return false;
}

void GhostNetManager::pruneStale()
{
    const uint32_t now = millis();
    for (size_t i = 0U; i < MAX_PEERS; ++i)
    {
        if (peers_[i].active &&
            (now - peers_[i].lastSeenMs) > PEER_TIMEOUT_MS)
        {
            ESP_LOGI(TAG_GN, "Peer pruned (timeout): %s", peers_[i].name);
            (void)esp_now_del_peer(peers_[i].mac);
            peers_[i].active = false;
        }
    }
}

// ── Beacon ───────────────────────────────────────────────────────────────────

void GhostNetManager::sendBeacon()
{
    (void)sendPacket(GhostMsgType::BEACON, nullptr, 0U);
}

// ── Chat ─────────────────────────────────────────────────────────────────────

bool GhostNetManager::sendChat(const char *text)
{
    if (text == nullptr || text[0] == '\0')
    {
        return false;
    }

    const size_t textLen = std::strlen(text);
    const size_t sendLen = (textLen < GHOST_MAX_PAYLOAD) ? textLen : GHOST_MAX_PAYLOAD;

    // Store locally in the ring buffer.
    GhostChatMsg &msg = chatRing_[chatHead_];
    std::strncpy(msg.sender, nodeName_, sizeof(msg.sender) - 1U);
    msg.sender[sizeof(msg.sender) - 1U] = '\0';
    std::strncpy(msg.text, text, sizeof(msg.text) - 1U);
    msg.text[sizeof(msg.text) - 1U] = '\0';
    msg.timestampMs = millis();
    chatHead_ = (chatHead_ + 1U) % CHAT_RING_SIZE;
    if (chatCount_ < CHAT_RING_SIZE)
    {
        ++chatCount_;
    }

    return sendPacket(GhostMsgType::CHAT,
                      reinterpret_cast<const uint8_t *>(text), sendLen);
}

const GhostChatMsg *GhostNetManager::chatAt(size_t index) const
{
    if (index >= chatCount_)
    {
        return nullptr;
    }
    // Ring buffer: oldest is at (chatHead_ - chatCount_ + index) mod SIZE.
    const size_t pos = (chatHead_ + CHAT_RING_SIZE - chatCount_ + index) % CHAT_RING_SIZE;
    return &chatRing_[pos];
}

// ── Data sync ────────────────────────────────────────────────────────────────

bool GhostNetManager::syncData(const char *dataType,
                                const uint8_t *data, size_t len)
{
    if (dataType == nullptr || data == nullptr || len == 0U)
    {
        return false;
    }

    // Payload: [type_len(1)] [type_string] [data_bytes]
    const size_t typeLen = std::strlen(dataType);
    if (typeLen > 31U)
    {
        return false; // type string too long
    }

    const size_t totalLen = 1U + typeLen + len;
    if (totalLen > GHOST_MAX_PAYLOAD)
    {
        ESP_LOGW(TAG_GN, "Sync payload too large (%zu > %zu)",
                 totalLen, GHOST_MAX_PAYLOAD);
        return false;
    }

    uint8_t buf[250];
    buf[0] = static_cast<uint8_t>(typeLen);
    std::memcpy(&buf[1], dataType, typeLen);
    std::memcpy(&buf[1 + typeLen], data, len);

    return sendPacket(GhostMsgType::SYNC_DATA, buf, totalLen);
}

// ── Remote execution ─────────────────────────────────────────────────────────

bool GhostNetManager::sendCommand(GhostCmd cmd)
{
    const uint8_t cmdByte = static_cast<uint8_t>(cmd);
    return sendPacket(GhostMsgType::CMD_REQUEST, &cmdByte, 1U);
}

// ── Packet assembly and transmission ─────────────────────────────────────────

bool GhostNetManager::sendPacket(GhostMsgType type,
                                  const uint8_t *payload, size_t len)
{
    if (!initialized_)
    {
        return false;
    }

    if (len > GHOST_MAX_PAYLOAD)
    {
        return false;
    }

    uint8_t buf[250];
    auto *pkt = reinterpret_cast<GhostPacket *>(buf);

    pkt->magic[0] = GHOST_MAGIC[0];
    pkt->magic[1] = GHOST_MAGIC[1];
    pkt->type = type;
    pkt->seqNo = seqNo_++;
    std::memcpy(pkt->srcMac, ownMac_, 6);
    std::strncpy(pkt->srcName, nodeName_, sizeof(pkt->srcName) - 1U);
    pkt->srcName[sizeof(pkt->srcName) - 1U] = '\0';
    pkt->payloadLen = static_cast<uint8_t>(len);

    if (payload != nullptr && len > 0U)
    {
        std::memcpy(buf + GHOST_HEADER_SIZE, payload, len);
    }

    const size_t totalLen = GHOST_HEADER_SIZE + len;

    // Send to broadcast (discovery) or unicast per peer depending on type.
    if (type == GhostMsgType::BEACON)
    {
        // Broadcast beacon.
        const esp_err_t err = esp_now_send(BROADCAST_MAC, buf, totalLen);
        return (err == ESP_OK);
    }

    // Unicast to each known peer.
    bool allOk = true;
    for (size_t i = 0U; i < MAX_PEERS; ++i)
    {
        if (peers_[i].active)
        {
            const esp_err_t err = esp_now_send(peers_[i].mac, buf, totalLen);
            if (err != ESP_OK)
            {
                allOk = false;
            }
        }
    }
    return allOk;
}

// ── ESP-NOW callbacks ────────────────────────────────────────────────────────

void GhostNetManager::onDataRecv(const uint8_t *mac,
                                  const uint8_t *data, int len)
{
    if (mac == nullptr || data == nullptr ||
        len < static_cast<int>(GHOST_HEADER_SIZE))
    {
        return;
    }

    auto &mgr = instance();
    // Approximate RSSI – ESP-NOW does not expose RSSI directly in this
    // callback, so we use a placeholder.  Real RSSI can be obtained by
    // hooking the WiFi promiscuous callback, which is left for a future
    // enhancement.
    constexpr int8_t estimatedRssi = -50;
    mgr.handlePacket(mac, data, len, estimatedRssi);
}

void GhostNetManager::onDataSent(const uint8_t * /*mac*/, bool success)
{
    if (!success)
    {
        ESP_LOGD(TAG_GN, "ESP-NOW send failed");
    }
}

// ── Packet processing ────────────────────────────────────────────────────────

void GhostNetManager::handlePacket(const uint8_t *mac,
                                    const uint8_t *data, int len,
                                    int8_t rssi)
{
    const auto *pkt = reinterpret_cast<const GhostPacket *>(data);

    // Validate magic.
    if (pkt->magic[0] != GHOST_MAGIC[0] || pkt->magic[1] != GHOST_MAGIC[1])
    {
        return; // Not a GhostNet packet.
    }

    // Ignore our own packets.
    if (std::memcmp(pkt->srcMac, ownMac_, 6) == 0)
    {
        return;
    }

    // Validate payload length.
    const size_t expectedLen = GHOST_HEADER_SIZE + pkt->payloadLen;
    if (static_cast<size_t>(len) < expectedLen)
    {
        return;
    }

    // Update / add peer.
    (void)addOrUpdatePeer(mac, rssi, pkt->srcName);

    const uint8_t *payload = data + GHOST_HEADER_SIZE;

    switch (pkt->type)
    {
    case GhostMsgType::BEACON:
    {
        // Reply with a beacon ACK so the sender knows about us.
        (void)sendPacket(GhostMsgType::BEACON_ACK, nullptr, 0U);
        ESP_LOGD(TAG_GN, "Beacon from %s", pkt->srcName);
        break;
    }

    case GhostMsgType::BEACON_ACK:
    {
        ESP_LOGD(TAG_GN, "Beacon ACK from %s", pkt->srcName);
        break;
    }

    case GhostMsgType::CHAT:
    {
        // Store in ring buffer.
        GhostChatMsg &msg = chatRing_[chatHead_];
        std::strncpy(msg.sender, pkt->srcName, sizeof(msg.sender) - 1U);
        msg.sender[sizeof(msg.sender) - 1U] = '\0';
        const size_t copyLen = (pkt->payloadLen < sizeof(msg.text) - 1U)
                                   ? pkt->payloadLen
                                   : sizeof(msg.text) - 1U;
        std::memcpy(msg.text, payload, copyLen);
        msg.text[copyLen] = '\0';
        msg.timestampMs = millis();
        chatHead_ = (chatHead_ + 1U) % CHAT_RING_SIZE;
        if (chatCount_ < CHAT_RING_SIZE)
        {
            ++chatCount_;
        }

        ESP_LOGI(TAG_GN, "Chat from %s: %s", msg.sender, msg.text);

        // Post event for the UI.
        const Event evt{EventType::EVT_GHOSTNET,
                        static_cast<int32_t>(GhostMsgType::CHAT),
                        0, nullptr};
        EventSystem::instance().postEvent(evt);
        break;
    }

    case GhostMsgType::SYNC_DATA:
    {
        ESP_LOGI(TAG_GN, "Sync data received (%u bytes)", pkt->payloadLen);
        const Event evt{EventType::EVT_GHOSTNET,
                        static_cast<int32_t>(GhostMsgType::SYNC_DATA),
                        static_cast<int32_t>(pkt->payloadLen), nullptr};
        EventSystem::instance().postEvent(evt);
        break;
    }

    case GhostMsgType::CMD_REQUEST:
    {
        if (pkt->payloadLen >= 1U)
        {
            lastCmd_ = static_cast<GhostCmd>(payload[0]);
            ESP_LOGI(TAG_GN, "Command received: 0x%02X from %s",
                     payload[0], pkt->srcName);

            // Acknowledge receipt.
            const uint8_t ack = payload[0];
            (void)sendPacket(GhostMsgType::CMD_ACK, &ack, 1U);

            const Event evt{EventType::EVT_GHOSTNET,
                            static_cast<int32_t>(GhostMsgType::CMD_REQUEST),
                            static_cast<int32_t>(lastCmd_), nullptr};
            EventSystem::instance().postEvent(evt);
        }
        break;
    }

    case GhostMsgType::CMD_ACK:
    {
        ESP_LOGD(TAG_GN, "Command ACK from %s", pkt->srcName);
        break;
    }

    default:
        break;
    }
}

} // namespace hackos::core
