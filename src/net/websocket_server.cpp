/**
 * @file websocket_server.cpp
 * @brief Async WebSocket server implementation for ESP32.
 *
 * Leverages the ESP-IDF httpd WebSocket support (CONFIG_HTTPD_WS_SUPPORT)
 * to provide non-blocking, frame-level message delivery to browser clients.
 */

#include "net/websocket_server.h"

#include <cstring>
#include <new>
#include <esp_log.h>

static constexpr const char *TAG_WS = "WS_Server";

namespace hackos {
namespace net {

// ── Singleton ────────────────────────────────────────────────────────────────

WebSocketServer &WebSocketServer::instance()
{
    static WebSocketServer inst;
    return inst;
}

WebSocketServer::WebSocketServer()
    : server_(nullptr)
{
    for (size_t i = 0U; i < MAX_CLIENTS; ++i)
    {
        clients_[i] = -1;
    }
}

// ── Public API ───────────────────────────────────────────────────────────────

bool WebSocketServer::start(httpd_handle_t server, const char *uri)
{
    if (server == nullptr || uri == nullptr)
    {
        return false;
    }
    server_ = server;

    httpd_uri_t wsUri = {};
    wsUri.uri      = uri;
    wsUri.method   = HTTP_GET;
    wsUri.handler  = wsHandler;
    wsUri.user_ctx = nullptr;
#ifdef CONFIG_HTTPD_WS_SUPPORT
    wsUri.is_websocket            = true;
    wsUri.handle_ws_control_frames = true;
#endif

    esp_err_t err = httpd_register_uri_handler(server_, &wsUri);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG_WS, "Failed to register WS handler: %d", err);
        return false;
    }

    ESP_LOGI(TAG_WS, "WebSocket endpoint registered: %s", uri);
    return true;
}

size_t WebSocketServer::broadcast(const char *data)
{
    if (data == nullptr || server_ == nullptr)
    {
        return 0U;
    }

    const size_t len = strlen(data);
    size_t sent = 0U;

    for (size_t i = 0U; i < MAX_CLIENTS; ++i)
    {
        const int fd = clients_[i];
        if (fd < 0)
        {
            continue;
        }

        httpd_ws_frame_t frame = {};
        frame.type    = HTTPD_WS_TYPE_TEXT;
        frame.payload = reinterpret_cast<uint8_t *>(const_cast<char *>(data));
        frame.len     = len;

        esp_err_t ret = httpd_ws_send_frame_async(server_, fd, &frame);
        if (ret == ESP_OK)
        {
            ++sent;
        }
        else
        {
            // Client likely disconnected – remove it.
            ESP_LOGW(TAG_WS, "Send failed to fd %d, removing", fd);
            clients_[i] = -1;
        }
    }

    return sent;
}

bool WebSocketServer::sendTo(int fd, const char *data)
{
    if (data == nullptr || server_ == nullptr || fd < 0)
    {
        return false;
    }

    httpd_ws_frame_t frame = {};
    frame.type    = HTTPD_WS_TYPE_TEXT;
    frame.payload = reinterpret_cast<uint8_t *>(const_cast<char *>(data));
    frame.len     = strlen(data);

    return httpd_ws_send_frame_async(server_, fd, &frame) == ESP_OK;
}

size_t WebSocketServer::clientCount() const
{
    size_t count = 0U;
    for (size_t i = 0U; i < MAX_CLIENTS; ++i)
    {
        if (clients_[i] >= 0)
        {
            ++count;
        }
    }
    return count;
}

void WebSocketServer::addClient(int fd)
{
    for (size_t i = 0U; i < MAX_CLIENTS; ++i)
    {
        if (clients_[i] < 0)
        {
            clients_[i] = fd;
            ESP_LOGI(TAG_WS, "WS client added: fd=%d", fd);
            return;
        }
    }
    ESP_LOGW(TAG_WS, "Max WS clients reached, rejecting fd=%d", fd);
}

void WebSocketServer::removeClient(int fd)
{
    for (size_t i = 0U; i < MAX_CLIENTS; ++i)
    {
        if (clients_[i] == fd)
        {
            clients_[i] = -1;
            ESP_LOGI(TAG_WS, "WS client removed: fd=%d", fd);
            return;
        }
    }
}

httpd_handle_t WebSocketServer::serverHandle() const
{
    return server_;
}

// ── WebSocket handler ────────────────────────────────────────────────────────

esp_err_t wsHandler(httpd_req_t *req)
{
    if (req->method == HTTP_GET)
    {
        // New WebSocket handshake – register client.
        int fd = httpd_req_to_sockfd(req);
        WebSocketServer::instance().addClient(fd);
        ESP_LOGI(TAG_WS, "WS handshake from fd=%d", fd);
        return ESP_OK;
    }

    // Receive incoming frame.
    httpd_ws_frame_t frame = {};
    frame.type = HTTPD_WS_TYPE_TEXT;

    // First call with len=0 to get frame length.
    esp_err_t ret = httpd_ws_recv_frame(req, &frame, 0);
    if (ret != ESP_OK)
    {
        return ret;
    }

    if (frame.len == 0U)
    {
        return ESP_OK;
    }

    // Allocate buffer and read payload.
    uint8_t *buf = new (std::nothrow) uint8_t[frame.len + 1U];
    if (buf == nullptr)
    {
        return ESP_ERR_NO_MEM;
    }

    frame.payload = buf;
    ret = httpd_ws_recv_frame(req, &frame, frame.len);
    if (ret != ESP_OK)
    {
        delete[] buf;
        return ret;
    }
    buf[frame.len] = '\0';

    // Handle close frame.
    if (frame.type == HTTPD_WS_TYPE_CLOSE)
    {
        int fd = httpd_req_to_sockfd(req);
        WebSocketServer::instance().removeClient(fd);
    }

    // Echo/ping responses could be handled here if needed.

    delete[] buf;
    return ESP_OK;
}

} // namespace net
} // namespace hackos
