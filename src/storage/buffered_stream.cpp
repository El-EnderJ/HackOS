#include "storage/buffered_stream.h"

#include <cstring>
#include <esp_log.h>

#include "storage/vfs.h"

static constexpr const char *TAG_BUF = "BufferedStream";

namespace hackos::storage {

// ═══════════════════════════════════════════════════════════════════════════
// BufferedReader
// ═══════════════════════════════════════════════════════════════════════════

BufferedReader::BufferedReader()
    : totalRead_(0U),
      fileSize_(0U),
      open_(false)
{
}

BufferedReader::~BufferedReader()
{
    close();
}

bool BufferedReader::begin(const char *path)
{
    close();

    file_ = VirtualFS::instance().open(path, "r");
    if (!file_)
    {
        ESP_LOGW(TAG_BUF, "reader: cannot open %s", path ? path : "(null)");
        return false;
    }

    fileSize_ = file_.size();
    totalRead_ = 0U;
    open_ = true;
    return true;
}

size_t BufferedReader::readChunk(uint8_t *buf, size_t maxLen)
{
    if (!open_ || buf == nullptr || maxLen == 0U)
    {
        return 0U;
    }

    const size_t n = file_.read(buf, maxLen);
    totalRead_ += n;

    if (n == 0U || totalRead_ >= fileSize_)
    {
        open_ = false;
        file_.close();
    }
    return n;
}

bool BufferedReader::isFinished() const { return !open_; }
size_t BufferedReader::totalSize() const { return fileSize_; }
size_t BufferedReader::bytesRead() const { return totalRead_; }

void BufferedReader::close()
{
    if (open_)
    {
        file_.close();
        open_ = false;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// BufferedWriter
// ═══════════════════════════════════════════════════════════════════════════

BufferedWriter::BufferedWriter()
    : buffered_(0U),
      totalWritten_(0U),
      open_(false)
{
}

BufferedWriter::~BufferedWriter()
{
    close();
}

bool BufferedWriter::begin(const char *path, bool append)
{
    close();

    const char *mode = append ? "a" : "w";
    file_ = VirtualFS::instance().open(path, mode);
    if (!file_)
    {
        ESP_LOGW(TAG_BUF, "writer: cannot open %s", path ? path : "(null)");
        return false;
    }

    buffered_ = 0U;
    totalWritten_ = 0U;
    open_ = true;
    return true;
}

bool BufferedWriter::write(const uint8_t *data, size_t len)
{
    if (!open_ || data == nullptr)
    {
        return false;
    }

    size_t offset = 0U;
    while (offset < len)
    {
        const size_t space = BUFFER_SIZE - buffered_;
        const size_t chunk = (len - offset < space) ? (len - offset) : space;

        std::memcpy(buffer_ + buffered_, data + offset, chunk);
        buffered_ += chunk;
        offset += chunk;

        if (buffered_ >= BUFFER_SIZE)
        {
            if (!flush())
            {
                return false;
            }
        }
    }
    return true;
}

bool BufferedWriter::flush()
{
    if (!open_ || buffered_ == 0U)
    {
        return true;
    }

    const size_t written = file_.write(buffer_, buffered_);
    if (written != buffered_)
    {
        ESP_LOGW(TAG_BUF, "writer: short write (%u/%u)",
                 static_cast<unsigned>(written),
                 static_cast<unsigned>(buffered_));
        buffered_ = 0U;
        return false;
    }

    totalWritten_ += written;
    buffered_ = 0U;
    return true;
}

void BufferedWriter::close()
{
    if (open_)
    {
        flush();
        file_.close();
        open_ = false;
    }
}

size_t BufferedWriter::bytesWritten() const { return totalWritten_; }

} // namespace hackos::storage
