/**
 * @file cloud_exfil.h
 * @brief Cloud exfiltration – Telegram/Discord webhook integration.
 *
 * Sends captured credentials and handshakes to a configured webhook
 * URL via HTTPS POST.  Supports both Telegram Bot API and Discord
 * webhook formats.
 *
 * Configuration is stored in `/ext/cloud.cfg` on the SD card:
 * @code
 * TYPE=TELEGRAM
 * TOKEN=123456:ABC-DEF
 * CHAT_ID=-100123456
 * @endcode
 * or
 * @code
 * TYPE=DISCORD
 * WEBHOOK=https://discord.com/api/webhooks/...
 * @endcode
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace hackos::net {

enum class WebhookType : uint8_t
{
    NONE = 0,
    TELEGRAM,
    DISCORD,
};

class CloudExfil
{
public:
    static CloudExfil &instance();

    /// @brief Load configuration from `/ext/cloud.cfg`.
    bool loadConfig();

    /// @brief Send a text message to the configured webhook.
    /// @return true if the HTTP POST succeeded (2xx status).
    bool send(const char *message);

    /// @brief Returns the configured webhook type.
    WebhookType type() const;

    /// @brief Returns true if a valid configuration is loaded.
    bool isConfigured() const;

private:
    static constexpr size_t MAX_TOKEN_LEN  = 128U;
    static constexpr size_t MAX_URL_LEN    = 256U;
    static constexpr size_t MAX_CHAT_ID    = 32U;
    static constexpr const char *CONFIG_PATH = "/ext/cloud.cfg";

    CloudExfil();

    WebhookType type_;
    char token_[MAX_TOKEN_LEN + 1U];
    char chatId_[MAX_CHAT_ID + 1U];
    char webhookUrl_[MAX_URL_LEN + 1U];
    bool configured_;
};

} // namespace hackos::net
