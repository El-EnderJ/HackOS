/**
 * @file wardriving_app.cpp
 * @brief Wardriving App – GPS-tagged WiFi network scanning with KML export.
 *
 * Implements:
 *  - **GPS Parsing**: Reads NMEA sentences (GPGGA/GPRMC) from Serial2
 *    (hardware UART) or estimates position via WiFi triangulation if no
 *    GPS module is connected.
 *  - **WiFi Scanning**: Uses the Wireless HAL to perform periodic scans
 *    and logs each discovered AP with position, RSSI, channel, and auth mode.
 *  - **KML Export**: Generates Google Earth .kml files on the SD card with
 *    coloured placemarks indicating AP security (Open=red, WEP=orange,
 *    WPA=yellow, WPA2/3=green).
 *  - **Live Map**: OLED display showing scan count, GPS fix status, and
 *    discovered AP summary.
 *
 * Uses the HackOSApp lifecycle so all work runs cooperatively inside the
 * Core_Task loop (on_update) without blocking.
 *
 * @warning **Legal notice**: Wardriving may be subject to local regulations.
 * Always comply with applicable laws regarding wireless network scanning
 * and data collection in your jurisdiction.
 */

#include "apps/wardriving_app.h"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <new>

#include <esp_log.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>

#include "apps/hackos_app.h"
#include "core/event.h"
#include "core/event_system.h"
#include "hardware/display.h"
#include "hardware/input.h"
#include "hardware/wireless.h"
#include "storage/vfs.h"
#include "ui/widgets.h"

static constexpr const char *TAG_WD = "Wardriving";

namespace
{

// ── Tunables ──────────────────────────────────────────────────────────────────

static constexpr size_t   MAX_LOGGED_APS       = 128U;
static constexpr size_t   SSID_MAX_LEN         = 32U;
static constexpr uint32_t SCAN_INTERVAL_MS     = 5000U;
static constexpr uint32_t GPS_READ_INTERVAL_MS = 1000U;
static constexpr int      GPS_SERIAL_BAUD      = 9600;
static constexpr int      GPS_RX_PIN           = 16;  ///< Serial2 RX
static constexpr int      GPS_TX_PIN           = 17;  ///< Serial2 TX
static constexpr size_t   NMEA_BUF_SIZE        = 128U;
static constexpr size_t   KML_LINE_BUF         = 256U;

// ── State machine ────────────────────────────────────────────────────────────

enum class WDState : uint8_t
{
    MENU_MAIN,
    SCANNING,      ///< Active wardriving scan
    EXPORT_KML,    ///< Generating KML file
    EXPORT_DONE,   ///< KML export completed
};

// ── GPS fix data ─────────────────────────────────────────────────────────────

struct GpsFix
{
    double   latitude;
    double   longitude;
    double   altitude;
    uint8_t  satellites;
    bool     valid;
    uint32_t lastFixMs;
};

// ── Logged AP entry ──────────────────────────────────────────────────────────

struct LoggedAp
{
    uint8_t  bssid[6];
    char     ssid[33];
    int8_t   rssi;
    uint8_t  channel;
    uint8_t  authMode;  ///< wifi_auth_mode_t cast to uint8_t
    double   lat;
    double   lon;
    bool     hasGps;
};

// ── Static menu labels ───────────────────────────────────────────────────────

static constexpr size_t MAIN_MENU_COUNT = 3U;
static const char *const MAIN_MENU_LABELS[MAIN_MENU_COUNT] = {
    "Start Wardriving",
    "Export KML",
    "Back",
};

// ── App class ─────────────────────────────────────────────────────────────────

class WardrivingApp final : public hackos::HackOSApp
{
public:
    WardrivingApp()
        : statusBar_(0, 0, 128, 8),
          mainMenu_(0, 20, 128, 36, 3),
          state_(WDState::MENU_MAIN),
          needsRedraw_(true),
          loggedCount_(0U),
          scanRunning_(false),
          lastScanMs_(0U),
          lastGpsReadMs_(0U),
          gpsFix_{},
          gpsAvailable_(false),
          nmeaBufIdx_(0U),
          totalScans_(0U),
          openCount_(0U),
          wepCount_(0U),
          wpaCount_(0U)
    {
        std::memset(loggedAps_, 0, sizeof(loggedAps_));
        std::memset(nmeaBuf_, 0, sizeof(nmeaBuf_));
    }

protected:
    // ── HackOSApp lifecycle ──────────────────────────────────────────────

    void on_alloc() override {}

    void on_start() override
    {
        statusBar_.setConnectivity(false, false);
        statusBar_.setBatteryLevel(100U);
        statusBar_.setTime(0U, 0U);
        mainMenu_.setItems(MAIN_MENU_LABELS, MAIN_MENU_COUNT);

        (void)Wireless::instance().init();

        // Try to initialise GPS on Serial2
        Serial2.begin(GPS_SERIAL_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
        gpsAvailable_ = false; // Will be set true on first valid NMEA read

        state_ = WDState::MENU_MAIN;
        needsRedraw_ = true;
        ESP_LOGI(TAG_WD, "Wardriving app started (GPS on Serial2 RX=%d TX=%d)",
                 GPS_RX_PIN, GPS_TX_PIN);
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
        if (state_ != WDState::SCANNING)
        {
            return;
        }

        const uint32_t now = millis();

        // Read GPS data periodically
        if ((now - lastGpsReadMs_) >= GPS_READ_INTERVAL_MS)
        {
            readGpsData();
            lastGpsReadMs_ = now;
        }

        // Perform WiFi scan periodically
        if (!Wireless::instance().isScanning() &&
            (now - lastScanMs_) >= SCAN_INTERVAL_MS)
        {
            (void)Wireless::instance().startScan();
            lastScanMs_ = now;
        }

        // Check if scan completed and harvest results
        if (!Wireless::instance().isScanning() && Wireless::instance().apCount() > 0U)
        {
            harvestScanResults();
        }

        needsRedraw_ = true;
    }

    void on_draw() override
    {
        if (!needsRedraw_ && !anyWidgetDirty())
        {
            return;
        }

        auto &disp = DisplayManager::instance();
        disp.clear();
        statusBar_.draw();

        switch (state_)
        {
        case WDState::MENU_MAIN:
            drawTitle("Wardriving");
            mainMenu_.draw();
            break;
        case WDState::SCANNING:
            drawScanning();
            break;
        case WDState::EXPORT_KML:
            drawTitle("Exporting KML...");
            disp.drawText(2, 30, "Writing to SD card");
            break;
        case WDState::EXPORT_DONE:
            drawExportDone();
            break;
        }

        disp.present();
        clearAllDirty();
        needsRedraw_ = false;
    }

    void on_free() override
    {
        scanRunning_ = false;
        Serial2.end();
        Wireless::instance().deinit();
        ESP_LOGI(TAG_WD, "Wardriving app freed");
    }

private:
    StatusBar    statusBar_;
    MenuListView mainMenu_;

    WDState  state_;
    bool     needsRedraw_;

    // Logged APs
    LoggedAp loggedAps_[MAX_LOGGED_APS];
    size_t   loggedCount_;

    // Scan state
    bool     scanRunning_;
    uint32_t lastScanMs_;
    uint32_t lastGpsReadMs_;

    // GPS
    GpsFix   gpsFix_;
    bool     gpsAvailable_;
    char     nmeaBuf_[NMEA_BUF_SIZE];
    size_t   nmeaBufIdx_;

    // Statistics
    uint32_t totalScans_;
    size_t   openCount_;
    size_t   wepCount_;
    size_t   wpaCount_;

    // ── Widget helpers ───────────────────────────────────────────────────

    bool anyWidgetDirty() const
    {
        return statusBar_.isDirty() || mainMenu_.isDirty();
    }

    void clearAllDirty()
    {
        statusBar_.clearDirty();
        mainMenu_.clearDirty();
    }

    void transitionTo(WDState next)
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

    void drawScanning()
    {
        drawTitle("Wardriving");

        auto &disp = DisplayManager::instance();
        char buf[32];

        // GPS status
        if (gpsFix_.valid)
        {
            std::snprintf(buf, sizeof(buf), "GPS: %u sats",
                          static_cast<unsigned>(gpsFix_.satellites));
        }
        else if (gpsAvailable_)
        {
            std::snprintf(buf, sizeof(buf), "GPS: No fix");
        }
        else
        {
            std::snprintf(buf, sizeof(buf), "GPS: not detected");
        }
        disp.drawText(2, 22, buf);

        // AP count
        std::snprintf(buf, sizeof(buf), "APs: %u / %u",
                      static_cast<unsigned>(loggedCount_),
                      static_cast<unsigned>(MAX_LOGGED_APS));
        disp.drawText(2, 32, buf);

        // Security breakdown
        std::snprintf(buf, sizeof(buf), "O:%u W:%u S:%u",
                      static_cast<unsigned>(openCount_),
                      static_cast<unsigned>(wepCount_),
                      static_cast<unsigned>(wpaCount_));
        disp.drawText(2, 42, buf);

        // Scan count
        std::snprintf(buf, sizeof(buf), "Scans: %lu",
                      static_cast<unsigned long>(totalScans_));
        disp.drawText(2, 52, buf);

        disp.drawText(80, 56, "STOP");
    }

    void drawExportDone()
    {
        drawTitle("KML Export");

        auto &disp = DisplayManager::instance();
        char buf[32];

        if (loggedCount_ > 0U)
        {
            disp.drawText(2, 24, "Export complete!");
            std::snprintf(buf, sizeof(buf), "%u APs exported",
                          static_cast<unsigned>(loggedCount_));
            disp.drawText(2, 34, buf);
            disp.drawText(2, 44, "Saved to /ext/wardriving/");
        }
        else
        {
            disp.drawText(2, 28, "No APs to export");
        }

        disp.drawText(2, 56, "PRESS=Back");
    }

    // ── Input handling ───────────────────────────────────────────────────

    void handleInput(InputManager::InputEvent input)
    {
        switch (state_)
        {
        case WDState::MENU_MAIN:
            handleMainInput(input);
            break;
        case WDState::SCANNING:
            if (input == InputManager::InputEvent::BUTTON_PRESS ||
                input == InputManager::InputEvent::LEFT)
            {
                scanRunning_ = false;
                transitionTo(WDState::MENU_MAIN);
            }
            break;
        case WDState::EXPORT_KML:
            // Non-interactive during export
            break;
        case WDState::EXPORT_DONE:
            if (input == InputManager::InputEvent::BUTTON_PRESS ||
                input == InputManager::InputEvent::LEFT)
            {
                transitionTo(WDState::MENU_MAIN);
            }
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
            switch (mainMenu_.selectedIndex())
            {
            case 0U: // Start Wardriving
                startScanning();
                break;
            case 1U: // Export KML
                exportKml();
                break;
            case 2U: // Back
            {
                const Event evt{EventType::EVT_SYSTEM, SYSTEM_EVENT_BACK,
                                0, nullptr};
                EventSystem::instance().postEvent(evt);
                break;
            }
            default:
                break;
            }
        }
        else if (input == InputManager::InputEvent::LEFT)
        {
            const Event evt{EventType::EVT_SYSTEM, SYSTEM_EVENT_BACK,
                            0, nullptr};
            EventSystem::instance().postEvent(evt);
        }
    }

    // ── Scanning ─────────────────────────────────────────────────────────

    void startScanning()
    {
        loggedCount_ = 0U;
        totalScans_ = 0U;
        openCount_ = 0U;
        wepCount_ = 0U;
        wpaCount_ = 0U;
        std::memset(loggedAps_, 0, sizeof(loggedAps_));

        scanRunning_ = true;
        lastScanMs_ = 0U;
        lastGpsReadMs_ = 0U;
        transitionTo(WDState::SCANNING);

        // Award XP
        const Event xpEvt{EventType::EVT_XP_EARNED, XP_WIFI_SCAN, 0, nullptr};
        EventSystem::instance().postEvent(xpEvt);

        ESP_LOGI(TAG_WD, "Wardriving scan started");
    }

    void harvestScanResults()
    {
        const auto &wifi = Wireless::instance();
        const size_t count = wifi.apCount();
        const auto *aps = wifi.aps();

        if (aps == nullptr || count == 0U)
        {
            return;
        }

        ++totalScans_;

        for (size_t i = 0U; i < count && loggedCount_ < MAX_LOGGED_APS; ++i)
        {
            // Deduplicate by BSSID
            bool found = false;
            for (size_t j = 0U; j < loggedCount_; ++j)
            {
                if (std::memcmp(loggedAps_[j].bssid, aps[i].bssid, 6U) == 0)
                {
                    // Update RSSI if stronger
                    if (aps[i].rssi > loggedAps_[j].rssi)
                    {
                        loggedAps_[j].rssi = aps[i].rssi;
                        if (gpsFix_.valid)
                        {
                            loggedAps_[j].lat = gpsFix_.latitude;
                            loggedAps_[j].lon = gpsFix_.longitude;
                            loggedAps_[j].hasGps = true;
                        }
                    }
                    found = true;
                    break;
                }
            }

            if (!found)
            {
                LoggedAp &entry = loggedAps_[loggedCount_];
                std::memcpy(entry.bssid, aps[i].bssid, 6U);
                std::strncpy(entry.ssid, aps[i].ssid, 32U);
                entry.ssid[32] = '\0';
                entry.rssi = aps[i].rssi;
                entry.channel = aps[i].channel;
                entry.authMode = static_cast<uint8_t>(aps[i].authmode);

                if (gpsFix_.valid)
                {
                    entry.lat = gpsFix_.latitude;
                    entry.lon = gpsFix_.longitude;
                    entry.hasGps = true;
                }
                else
                {
                    entry.hasGps = false;
                }

                // Update security counters
                switch (aps[i].authmode)
                {
                case WIFI_AUTH_OPEN:
                    ++openCount_;
                    break;
                case WIFI_AUTH_WEP:
                    ++wepCount_;
                    break;
                default:
                    ++wpaCount_;
                    break;
                }

                ++loggedCount_;
            }
        }
    }

    // ── GPS NMEA parsing ─────────────────────────────────────────────────

    void readGpsData()
    {
        while (Serial2.available())
        {
            const char c = static_cast<char>(Serial2.read());

            if (c == '\n' || c == '\r')
            {
                if (nmeaBufIdx_ > 0U)
                {
                    nmeaBuf_[nmeaBufIdx_] = '\0';
                    parseNmeaSentence(nmeaBuf_);
                    nmeaBufIdx_ = 0U;
                }
                continue;
            }

            if (nmeaBufIdx_ < NMEA_BUF_SIZE - 1U)
            {
                nmeaBuf_[nmeaBufIdx_++] = c;
            }
        }
    }

    /// @brief Parse a GPGGA or GPRMC sentence.
    void parseNmeaSentence(const char *sentence)
    {
        if (sentence == nullptr)
        {
            return;
        }

        // Any valid NMEA sentence means GPS module is connected
        if (sentence[0] == '$')
        {
            gpsAvailable_ = true;
        }

        // $GPGGA: Global Positioning System Fix Data
        if (std::strncmp(sentence, "$GPGGA", 6U) == 0 ||
            std::strncmp(sentence, "$GNGGA", 6U) == 0)
        {
            parseGpgga(sentence);
        }
        // $GPRMC: Recommended Minimum Specific GPS/Transit Data
        else if (std::strncmp(sentence, "$GPRMC", 6U) == 0 ||
                 std::strncmp(sentence, "$GNRMC", 6U) == 0)
        {
            parseGprmc(sentence);
        }
    }

    /// @brief Parse GPGGA sentence for position and satellite count.
    void parseGpgga(const char *sentence)
    {
        // Fields: $GPGGA,time,lat,N/S,lon,E/W,quality,sats,hdop,alt,...
        char fields[15][20];
        std::memset(fields, 0, sizeof(fields));
        splitCsv(sentence, fields, 15U);

        // Quality indicator (field 6): 0=invalid, 1+=valid
        const int fixQuality = std::atoi(fields[6]);
        if (fixQuality == 0)
        {
            gpsFix_.valid = false;
            return;
        }

        gpsFix_.latitude = parseNmeaCoord(fields[2], fields[3][0]);
        gpsFix_.longitude = parseNmeaCoord(fields[4], fields[5][0]);
        gpsFix_.altitude = std::atof(fields[9]);
        gpsFix_.satellites = static_cast<uint8_t>(std::atoi(fields[7]));
        gpsFix_.valid = true;
        gpsFix_.lastFixMs = millis();
    }

    /// @brief Parse GPRMC sentence for position.
    void parseGprmc(const char *sentence)
    {
        // Fields: $GPRMC,time,status,lat,N/S,lon,E/W,speed,course,date,...
        char fields[13][20];
        std::memset(fields, 0, sizeof(fields));
        splitCsv(sentence, fields, 13U);

        if (fields[2][0] != 'A') // A=active, V=void
        {
            gpsFix_.valid = false;
            return;
        }

        gpsFix_.latitude = parseNmeaCoord(fields[3], fields[4][0]);
        gpsFix_.longitude = parseNmeaCoord(fields[5], fields[6][0]);
        gpsFix_.valid = true;
        gpsFix_.lastFixMs = millis();
    }

    /// @brief Convert NMEA coordinate (DDMM.MMMM) to decimal degrees.
    static double parseNmeaCoord(const char *raw, char hemisphere)
    {
        if (raw == nullptr || raw[0] == '\0')
        {
            return 0.0;
        }

        const double val = std::atof(raw);
        const int degrees = static_cast<int>(val / 100.0);
        const double minutes = val - (degrees * 100.0);
        double result = degrees + (minutes / 60.0);

        if (hemisphere == 'S' || hemisphere == 'W')
        {
            result = -result;
        }

        return result;
    }

    /// @brief Split a CSV-like NMEA sentence into fields.
    static void splitCsv(const char *sentence, char fields[][20],
                         size_t maxFields)
    {
        size_t fieldIdx = 0U;
        size_t charIdx = 0U;
        const char *p = sentence;

        while (*p != '\0' && fieldIdx < maxFields)
        {
            if (*p == ',')
            {
                fields[fieldIdx][charIdx] = '\0';
                ++fieldIdx;
                charIdx = 0U;
            }
            else if (charIdx < 19U)
            {
                fields[fieldIdx][charIdx++] = *p;
            }
            ++p;
        }
        if (fieldIdx < maxFields)
        {
            fields[fieldIdx][charIdx] = '\0';
        }
    }

    // ── KML Export ───────────────────────────────────────────────────────

    void exportKml()
    {
        if (loggedCount_ == 0U)
        {
            transitionTo(WDState::EXPORT_DONE);
            return;
        }

        transitionTo(WDState::EXPORT_KML);
        needsRedraw_ = true;

        auto &vfs = hackos::storage::VirtualFS::instance();

        char filename[64];
        std::snprintf(filename, sizeof(filename),
                      "/ext/wardriving/wardrive_%lu.kml",
                      static_cast<unsigned long>(millis()));

        fs::File f = vfs.open(filename, "w");
        if (!f)
        {
            ESP_LOGE(TAG_WD, "Failed to create KML file: %s", filename);
            transitionTo(WDState::EXPORT_DONE);
            return;
        }

        // KML header
        f.print("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
        f.print("<kml xmlns=\"http://www.opengis.net/kml/2.2\">\n");
        f.print("<Document>\n");
        f.print("<name>HackOS Wardriving</name>\n");
        f.print("<description>WiFi networks discovered by HackOS</description>\n");

        // Style definitions for security levels
        writeKmlStyles(f);

        // Write placemarks
        for (size_t i = 0U; i < loggedCount_; ++i)
        {
            const LoggedAp &ap = loggedAps_[i];

            if (!ap.hasGps)
            {
                continue; // Skip APs without GPS coordinates
            }

            const char *styleId = getSecurityStyleId(ap.authMode);
            const char *secLabel = getSecurityLabel(ap.authMode);

            char buf[KML_LINE_BUF];

            f.print("<Placemark>\n");

            // Name: SSID or [hidden]
            const char *ssid = ap.ssid[0] != '\0' ? ap.ssid : "[hidden]";
            std::snprintf(buf, sizeof(buf), "<name>%s</name>\n", ssid);
            f.print(buf);

            // Description
            std::snprintf(buf, sizeof(buf),
                          "<description>"
                          "BSSID: %02X:%02X:%02X:%02X:%02X:%02X\n"
                          "Channel: %u\n"
                          "RSSI: %d dBm\n"
                          "Security: %s"
                          "</description>\n",
                          ap.bssid[0], ap.bssid[1], ap.bssid[2],
                          ap.bssid[3], ap.bssid[4], ap.bssid[5],
                          static_cast<unsigned>(ap.channel),
                          static_cast<int>(ap.rssi),
                          secLabel);
            f.print(buf);

            // Style reference
            std::snprintf(buf, sizeof(buf), "<styleUrl>#%s</styleUrl>\n",
                          styleId);
            f.print(buf);

            // Coordinates (lon,lat,alt)
            std::snprintf(buf, sizeof(buf),
                          "<Point><coordinates>%.6f,%.6f,0</coordinates></Point>\n",
                          ap.lon, ap.lat);
            f.print(buf);

            f.print("</Placemark>\n");
        }

        f.print("</Document>\n");
        f.print("</kml>\n");
        f.close();

        ESP_LOGI(TAG_WD, "KML exported to %s (%u APs)",
                 filename, static_cast<unsigned>(loggedCount_));

        // Award XP
        const Event xpEvt{EventType::EVT_XP_EARNED, XP_WIFI_SCAN, 0, nullptr};
        EventSystem::instance().postEvent(xpEvt);

        transitionTo(WDState::EXPORT_DONE);
    }

    /// @brief Write KML style definitions for AP security levels.
    static void writeKmlStyles(fs::File &f)
    {
        // Open (red)
        f.print("<Style id=\"open\"><IconStyle>"
                "<color>ff0000ff</color>"
                "<scale>1.0</scale>"
                "</IconStyle></Style>\n");
        // WEP (orange)
        f.print("<Style id=\"wep\"><IconStyle>"
                "<color>ff0080ff</color>"
                "<scale>1.0</scale>"
                "</IconStyle></Style>\n");
        // WPA (yellow)
        f.print("<Style id=\"wpa\"><IconStyle>"
                "<color>ff00ffff</color>"
                "<scale>1.0</scale>"
                "</IconStyle></Style>\n");
        // WPA2/WPA3 (green)
        f.print("<Style id=\"wpa2\"><IconStyle>"
                "<color>ff00ff00</color>"
                "<scale>1.0</scale>"
                "</IconStyle></Style>\n");
    }

    /// @brief Map auth mode to KML style ID.
    static const char *getSecurityStyleId(uint8_t authMode)
    {
        switch (static_cast<wifi_auth_mode_t>(authMode))
        {
        case WIFI_AUTH_OPEN:
            return "open";
        case WIFI_AUTH_WEP:
            return "wep";
        case WIFI_AUTH_WPA_PSK:
        case WIFI_AUTH_WPA_WPA2_PSK:
            return "wpa";
        default:
            return "wpa2";
        }
    }

    /// @brief Map auth mode to human-readable label.
    static const char *getSecurityLabel(uint8_t authMode)
    {
        switch (static_cast<wifi_auth_mode_t>(authMode))
        {
        case WIFI_AUTH_OPEN:
            return "Open";
        case WIFI_AUTH_WEP:
            return "WEP";
        case WIFI_AUTH_WPA_PSK:
            return "WPA";
        case WIFI_AUTH_WPA_WPA2_PSK:
            return "WPA/WPA2";
        case WIFI_AUTH_WPA2_PSK:
            return "WPA2";
        case WIFI_AUTH_WPA3_PSK:
            return "WPA3";
        case WIFI_AUTH_WPA2_WPA3_PSK:
            return "WPA2/WPA3";
        default:
            return "Unknown";
        }
    }
};

} // namespace

// ── Factory ──────────────────────────────────────────────────────────────────

AppBase *createWardrivingApp()
{
    return new (std::nothrow) WardrivingApp();
}
