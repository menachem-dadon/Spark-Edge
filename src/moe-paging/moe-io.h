// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright (c) 2026 SparkMoE contributors.

#pragma once

#include <cstddef>
#include <cstdint>
#include <future>
#include <functional>
#include <memory>
#include <string>

namespace llama_moe {

struct io_result {
    bool ok = false;
    uint64_t bytes = 0;
    uint64_t latency_us = 0;
    std::string error;
    uint64_t physical_bytes = 0;
    uint64_t direct_bytes = 0;
    uint64_t fallback_reads = 0;
    uint64_t queue_us = 0;
};

class file_reader {
public:
    virtual ~file_reader() = default;
    virtual io_result read_exact(uint64_t offset, void * destination, size_t length, uint32_t timeout_ms) = 0;
    virtual uint64_t size() const = 0;
    virtual size_t alignment() const { return 1; }
    virtual bool direct_enabled() const { return false; }
};

std::shared_ptr<file_reader> make_file_reader(int native_fd, uint64_t file_size, bool direct = false);
std::shared_ptr<file_reader> make_native_file_reader(int native_fd, uint64_t file_size, bool direct);
std::shared_ptr<file_reader> make_aligned_reader(std::shared_ptr<file_reader> buffered, std::shared_ptr<file_reader> direct);

enum class io_priority { demand, prefetch };

class io_executor {
public:
    explicit io_executor(uint32_t thread_count);
    ~io_executor();

    io_executor(const io_executor &) = delete;
    io_executor & operator=(const io_executor &) = delete;

    std::future<io_result> submit(std::function<io_result()> task, io_priority priority = io_priority::demand, uint64_t tag = 0);
    bool promote(uint64_t tag);
    bool cancel(uint64_t tag);

private:
    struct impl;
    std::unique_ptr<impl> pimpl;
};

}
