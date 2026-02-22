#pragma once

#include <cstdint>
#include <esp_event.h>
#include <esp_wifi_types.h>

/**
 * @file wireless.h
 * @brief HAL interface for WiFi operations (scan, deauth).
 *
 * All ESP-IDF WiFi calls are encapsulated here so that upper layers
 * (apps) remain independent of the hardware details.
 */

class Wireless
{
public:
    static constexpr uint8_t MAX_APS = 16U;

    struct ApRecord
    {
        char ssid[33];        ///< Null-terminated SSID (max 32 chars + NUL)
        uint8_t bssid[6];     ///< BSSID (raw 6-byte MAC)
        int8_t rssi;          ///< Signal strength in dBm
        uint8_t channel;      ///< Primary channel
        wifi_auth_mode_t authmode; ///< Authentication mode
    };

    static Wireless &instance();

    /// @brief Initialise WiFi in STA mode and register scan-done handler.
    bool init();

    /// @brief Stop WiFi, unregister handlers and free scan results.
    void deinit();

    /// @brief Kick off a non-blocking scan. Posts EVT_WIFI_SCAN_DONE when done.
    bool startScan();

    bool isScanning() const;

    uint8_t apCount() const;

    /// @brief Returns the dynamically-allocated AP record array (may be nullptr).
    const ApRecord *aps() const;

    /**
     * @brief Send IEEE 802.11 deauthentication frames towards an AP.
     *
     * Frames are sent via esp_wifi_80211_tx with broadcast DA so that all
     * associated clients receive them.
     *
     * @warning **Legal notice**: Sending deauthentication frames against
     * networks or devices you do not own or have explicit written permission
     * to test is illegal in most jurisdictions (e.g. CFAA, EU Directive
     * 2013/40/EU, and equivalents worldwide).  Use exclusively on networks
     * under your own control or in an authorised penetration-testing
     * engagement.
     *
     * @param bssid   6-byte BSSID of the target AP.
     * @param channel Channel of the target AP.
     * @param count   Number of frames to send in this call.
     * @return true if all frames were submitted to the radio.
     */
    bool sendDeauth(const uint8_t *bssid, uint8_t channel, uint8_t count);

private:
    Wireless();

    static void wifiEventHandler(void *arg, esp_event_base_t eventBase,
                                 int32_t eventId, void *eventData);
    void onScanDone();

    bool initialized_;
    bool scanning_;
    uint8_t apCount_;
    ApRecord *aps_;  ///< Heap-allocated; nullptr until first scan completes
    esp_event_handler_instance_t instanceHandle_;
};
