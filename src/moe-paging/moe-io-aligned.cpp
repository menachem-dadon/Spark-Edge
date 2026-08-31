// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright (c) 2026 SparkMoE contributors.
#include "moe-io.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <new>
#include <stdexcept>
#include <vector>

namespace llama_moe {

struct aligned_bytes {
    aligned_bytes(size_t size, size_t alignment) : alignment(alignment),
        data(static_cast<uint8_t *>(::operator new(size, std::align_val_t(alignment)))) {}
    ~aligned_bytes() { ::operator delete(data, std::align_val_t(alignment)); }
    size_t alignment;
    uint8_t * data;
};

class aligned_reader final : public file_reader {
public:
    aligned_reader(std::shared_ptr<file_reader> buffered, std::shared_ptr<file_reader> direct) :
        buffered(std::move(buffered)), direct(std::move(direct)) {
        if (!this->buffered) { throw std::invalid_argument("missing buffered reader"); }
        if (!this->direct || this->direct->size() != size()) { return; }
        unit = std::max<size_t>(4096, this->direct->alignment());
        if ((unit & (unit - 1)) || unit > 1024 * 1024 || size() < unit) { return; }
        aligned_bytes probe(unit, unit);
        std::vector<uint8_t> expected(unit);
        const auto a = this->buffered->read_exact(0, expected.data(), unit, 30000);
        const auto b = this->direct->read_exact(0, probe.data, unit, 30000);
        enabled = a.ok && b.ok && !std::memcmp(expected.data(), probe.data, unit);
    }

    uint64_t size() const override { return buffered->size(); }
    size_t alignment() const override { return unit; }
    bool direct_enabled() const override { return enabled.load(); }

    io_result read_exact(uint64_t offset, void * destination, size_t length, uint32_t timeout) override {
        if (offset > size() || length > size() - offset) {
            io_result failed;
            failed.error = "read range is outside the GGUF shard";
            return failed;
        }
        if (!enabled.load()) {
            auto result = buffered->read_exact(offset, destination, length, timeout);
            result.fallback_reads += length ? 1 : 0;
            return result;
        }
        const auto started = std::chrono::steady_clock::now();
        const size_t capacity = 8 * 1024 * 1024;
        aligned_bytes bounce(std::min(capacity, ((std::min(length, capacity) + unit - 1) / unit + 1) * unit), unit);
        io_result result;
        auto * output = static_cast<uint8_t *>(destination);
        while (result.bytes < length) {
            const uint64_t position = offset + result.bytes;
            const uint64_t aligned_offset = position / unit * unit;
            const size_t prefix = static_cast<size_t>(position - aligned_offset);
            const size_t wanted = std::min<size_t>(length - result.bytes, capacity - prefix);
            const size_t span = (prefix + wanted + unit - 1) / unit * unit;
            io_result part;
            if (enabled.load() && span <= size() - aligned_offset) {
                part = direct->read_exact(aligned_offset, bounce.data, span, timeout);
                result.physical_bytes += part.bytes;
                if (part.ok) {
                    std::memcpy(output + result.bytes, bounce.data + prefix, wanted);
                    result.direct_bytes += span;
                    result.bytes += wanted;
                    continue;
                }
                enabled = false;
            }
            // A shard tail need not be sector-aligned; never read past EOF.
            part = buffered->read_exact(position, output + result.bytes, wanted, timeout);
            ++result.fallback_reads;
            result.physical_bytes += part.bytes;
            if (!part.ok) { result.error = part.error; return result; }
            result.bytes += wanted;
        }
        result.ok = true;
        result.latency_us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started).count());
        return result;
    }

private:
    std::shared_ptr<file_reader> buffered;
    std::shared_ptr<file_reader> direct;
    size_t unit = 4096;
    std::atomic<bool> enabled{false};
};

std::shared_ptr<file_reader> make_aligned_reader(std::shared_ptr<file_reader> buffered, std::shared_ptr<file_reader> direct) {
    return std::make_shared<aligned_reader>(std::move(buffered), std::move(direct));
}

std::shared_ptr<file_reader> make_file_reader(int fd, uint64_t size, bool direct) {
    auto buffered = make_native_file_reader(fd, size, false);
    if (!direct) { return buffered; }
    std::shared_ptr<file_reader> raw;
    try { raw = make_native_file_reader(fd, size, true); }
    catch (const std::runtime_error &) { /* Unsupported filesystems use the buffered handle. */ }
    return make_aligned_reader(std::move(buffered), std::move(raw));
}

}
