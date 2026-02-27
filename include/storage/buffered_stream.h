/**
 * @file buffered_stream.h
 * @brief Chunked, non-blocking file I/O for large captures.
 *
 * Provides BufferedReader and BufferedWriter classes that process files
 * one chunk at a time, allowing the caller to yield between chunks so
 * that critical FreeRTOS tasks (radio capture, UI rendering) are not
 * starved.
 *
 * Typical write pattern (e.g. PCAP capture):
 * @code
 * hackos::storage::BufferedWriter writer;
 * writer.begin("/ext/captures/scan.pcap");
 * while (capturing) {
 *     writer.write(packetBuf, packetLen);   // buffers internally
 *     vTaskDelay(pdMS_TO_TICKS(1));         // yield
 * }
 * writer.close();                           // flushes remaining data
 * @endcode
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <FS.h>

namespace hackos::storage {

// ── BufferedReader ────────────────────────────────────────────────────────────

/**
 * @brief Reads a file one chunk at a time.
 */
class BufferedReader
{
public:
    BufferedReader();
    ~BufferedReader();

    /**
     * @brief Open a file for chunked reading.
     * @param path  Virtual path (routed through VirtualFS).
     * @return true if the file was opened successfully.
     */
    bool begin(const char *path);

    /**
     * @brief Read the next chunk of data.
     * @param buf     Destination buffer provided by the caller.
     * @param maxLen  Maximum bytes to read in this chunk.
     * @return Number of bytes actually read (0 when finished or on error).
     */
    size_t readChunk(uint8_t *buf, size_t maxLen);

    /// @brief True when the entire file has been read.
    bool isFinished() const;

    /// @brief Total file size in bytes.
    size_t totalSize() const;

    /// @brief Cumulative bytes read so far.
    size_t bytesRead() const;

    /// @brief Close the file (safe to call multiple times).
    void close();

private:
    fs::File file_;
    size_t totalRead_;
    size_t fileSize_;
    bool open_;
};

// ── BufferedWriter ───────────────────────────────────────────────────────────

/**
 * @brief Accumulates data in an internal buffer and flushes to disk.
 *
 * Writes are batched into BUFFER_SIZE chunks to minimise the number of
 * SD-card write transactions, which are the most time-consuming operation.
 */
class BufferedWriter
{
public:
    BufferedWriter();
    ~BufferedWriter();

    /**
     * @brief Open (or create) a file for chunked writing.
     * @param path    Virtual path (routed through VirtualFS).
     * @param append  When true the file is opened in append mode.
     * @return true if the file was opened successfully.
     */
    bool begin(const char *path, bool append = false);

    /**
     * @brief Append data to the internal buffer.
     *
     * If the buffer fills up it is automatically flushed to disk.
     *
     * @return true on success.
     */
    bool write(const uint8_t *data, size_t len);

    /**
     * @brief Force the internal buffer to be written to disk.
     * @return true on success.
     */
    bool flush();

    /// @brief Flush remaining data and close the file.
    void close();

    /// @brief Cumulative bytes written to disk.
    size_t bytesWritten() const;

private:
    /// Internal buffer size – balances RAM usage vs. write efficiency.
    static constexpr size_t BUFFER_SIZE = 512U;

    fs::File file_;
    uint8_t buffer_[BUFFER_SIZE];
    size_t buffered_;
    size_t totalWritten_;
    bool open_;
};

} // namespace hackos::storage
