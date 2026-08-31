// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright (c) 2026 SparkMoE contributors.

#include "moe-io.h"

#if !defined(_WIN32)

#include <cerrno>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <limits>

namespace llama_moe {

class posix_file_reader final : public file_reader {
public:
    posix_file_reader(int native_fd, uint64_t file_size, bool direct) : file_size(file_size) {
        if (direct) {
#if defined(__linux__) && defined(O_DIRECT)
            // dup + F_SETFL would also change the upstream loader's file description.
            const std::string path = "/proc/self/fd/" + std::to_string(native_fd);
            fd = open(path.c_str(), O_RDONLY | O_DIRECT | O_CLOEXEC);
            struct stat source{}, reopened{};
            if (fd >= 0 && (fstat(native_fd, &source) || fstat(fd, &reopened) ||
                    source.st_dev != reopened.st_dev || source.st_ino != reopened.st_ino)) {
                close(fd);
                fd = -1;
            }
#else
            throw std::runtime_error("direct reads are unavailable on this platform");
#endif
        } else {
            fd = dup(native_fd);
        }
        if (fd < 0) {
            throw std::runtime_error(std::string("open expert reader failed: ") + std::strerror(errno));
        }
    }

    ~posix_file_reader() override {
        close(fd);
    }

    io_result read_exact(uint64_t offset, void * destination, size_t length, uint32_t timeout_ms) override {
        io_result result;
        if (offset > file_size || length > file_size - offset ||
                offset > static_cast<uint64_t>(std::numeric_limits<off_t>::max())) {
            result.error = "read range is outside the GGUF shard";
            return result;
        }

        const auto started = std::chrono::steady_clock::now();
        auto * output = static_cast<uint8_t *>(destination);
        size_t completed = 0;
        while (completed < length) {
            const ssize_t count = pread(fd, output + completed, length - completed, static_cast<off_t>(offset + completed));
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count < 0) {
                result.error = std::string("pread failed: ") + std::strerror(errno);
                return result;
            }
            if (count == 0) {
                result.error = "unexpected EOF while reading an expert tensor";
                return result;
            }
            completed += static_cast<size_t>(count);
        }

        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started).count();
        result.ok = true;
        result.bytes = completed;
        result.physical_bytes = completed;
        result.latency_us = static_cast<uint64_t>(elapsed);
        if (timeout_ms > 0 && result.latency_us > static_cast<uint64_t>(timeout_ms) * 1000) {
            result.ok = false;
            result.error = "expert read exceeded the configured timeout";
        }
        return result;
    }

    uint64_t size() const override { return file_size; }
    size_t alignment() const override { return 4096; }

private:
    int fd = -1;
    uint64_t file_size = 0;
};

std::shared_ptr<file_reader> make_native_file_reader(int native_fd, uint64_t file_size, bool direct) {
    return std::make_shared<posix_file_reader>(native_fd, file_size, direct);
}

}

#endif
