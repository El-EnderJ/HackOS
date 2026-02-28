/**
 * @file websocket_server.h
 * @brief Asynchronous WebSocket server for high-frequency data streaming.
 *
 * Provides a lightweight WebSocket layer on top of the ESP-IDF HTTP server.
 * Used to push live RF signal samples and WiFi PCAP metadata to the
 * Web Command Center without blocking the application loop.
 *
 * Usage:
 * @code
 *   auto &ws = hackos::net::WebSocketServer::instance();
 *   ws.start(server_handle);
 *   ws.broadcast("{\"rssi\":-42}");
 * @endcode
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include <esp_http_server.h>

namespace hackos {
namespace net {

class WebSocketServer
{
public:
    static WebSocketServer &instance();

    /// Maximum simultaneously tracked client file descriptors.
    static constexpr size_t MAX_CLIENTS = 4U;

    /**
     * @brief Register the WebSocket URI handler on the given HTTP server.
     * @param server  A running httpd_handle_t.
     * @param uri     The WebSocket endpoint path (default "/ws").
     * @return true on success.
     */
    bool start(httpd_handle_t server, const char *uri = "/ws");

    /**
     * @brief Broadcast a text message to all connected WS clients.
     *
     * Non-blocking: if a client's socket buffer is full the frame is
     * silently dropped for that client.
     *
     * @param data  NUL-terminated string to send.
     * @return Number of clients the message was sent to.
     */
    size_t broadcast(const char *data);

    /**
     * @brief Send a text message to a specific client.
     * @param fd   Client file descriptor.
     * @param data NUL-terminated payload.
     * @return true if the frame was queued successfully.
     */
    bool sendTo(int fd, const char *data);

    /// @return current number of connected WS clients.
    size_t clientCount() const;

    /// @brief Remove a client (called internally on disconnect).
    void removeClient(int fd);

    /// @brief Add a client (called internally on new connection).
    void addClient(int fd);

    /// @brief Get the httpd_handle for use in async send.
    httpd_handle_t serverHandle() const;

private:
    WebSocketServer();

    httpd_handle_t server_;
    int clients_[MAX_CLIENTS];
    size_t clientCount_;
};

/// Internal handler – must be visible to register with httpd.
esp_err_t wsHandler(httpd_req_t *req);

} // namespace net
} // namespace hackos
