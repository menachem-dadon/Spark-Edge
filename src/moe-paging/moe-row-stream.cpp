// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright (c) 2026 SparkMoE contributors.
#include "moe-row-stream.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace llama_moe {
row_stream::row_stream(std::vector<row_span> spans, size_t row_bytes, uint64_t budget,
                       size_t max_inflight_rows, std::shared_ptr<io_executor> executor, uint32_t timeout_ms) :
    spans(std::move(spans)), row_bytes(row_bytes), max_inflight(max_inflight_rows), timeout(timeout_ms), executor(std::move(executor)) {
    if (!row_bytes || row_bytes > SIZE_MAX - sizeof(entry) || !max_inflight || max_inflight > 32 || !this->executor ||
            this->spans.empty() || this->spans.size() > 65536) {
        throw std::invalid_argument("invalid bounded row stream configuration");
    }
    std::sort(this->spans.begin(), this->spans.end(), [](const row_span & a, const row_span & b) { return a.first_row < b.first_row; });
    for (const auto & span : this->spans) {
        if (!span.reader || !span.row_count || span.first_row != rows || span.row_count > UINT64_MAX - rows ||
                span.row_stride < row_bytes || span.file_offset > span.reader->size() ||
                row_bytes > span.reader->size() - span.file_offset ||
                span.row_count - 1 > (span.reader->size() - span.file_offset - row_bytes) / span.row_stride) {
            throw std::invalid_argument("invalid, overlapping, missing or out-of-file row span");
        }
        rows += span.row_count;
    }
    const uint64_t metadata_bytes = this->spans.capacity() * sizeof(row_span);
    if (budget <= metadata_bytes) { throw std::invalid_argument("row budget does not cover source metadata"); }
    const uint64_t capacity = std::min<uint64_t>({65536, rows, (budget - metadata_bytes) / (row_bytes + sizeof(entry))});
    if (!capacity || capacity > SIZE_MAX / row_bytes) { throw std::invalid_argument("row budget cannot hold one row"); }
    slots = static_cast<size_t>(capacity);
    max_inflight = std::min(max_inflight, slots);
    entries = std::make_unique<entry[]>(slots);
    data = std::make_unique<uint8_t[]>(slots * row_bytes);
    counters.allocated_bytes = metadata_bytes + slots * (row_bytes + sizeof(entry));
}

const row_span & row_stream::source(uint64_t row) const {
    const auto it = std::upper_bound(spans.begin(), spans.end(), row,
        [](uint64_t value, const row_span & span) { return value < span.first_row; });
    if (it == spans.begin()) { throw std::out_of_range("row has no source"); }
    return *(it - 1);
}

void row_stream::copy_rows(const uint64_t * ids, size_t count, void * output, size_t output_bytes) {
    if (!ids || !output || !count || count > 256 || count > SIZE_MAX / row_bytes || output_bytes < count * row_bytes) {
        throw std::invalid_argument("bounded gather requires 1..256 rows and sufficient caller-owned output");
    }
    for (size_t i = 0; i < count; ++i) { if (ids[i] >= rows) { throw std::out_of_range("row ID is outside the tensor"); } }
    std::lock_guard<std::mutex> lock(mutex);
    for (size_t begin = 0; begin < count; begin += max_inflight) {
        const size_t n = std::min(max_inflight, count - begin);
        std::vector<size_t> mapped(n, slots), reserved;
        std::vector<std::pair<size_t, uint64_t>> missing;
        std::vector<std::future<io_result>> futures;
        reserved.reserve(n); missing.reserve(n); futures.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            for (size_t slot = 0; slot < slots; ++slot) {
                if (entries[slot].row == ids[begin + i]) {
                    mapped[i] = slot; reserved.push_back(slot); ++counters.cache_hits; break;
                }
            }
        }
        for (size_t i = 0; i < n; ++i) {
            if (mapped[i] != slots) { entries[mapped[i]].age = ++clock; continue; }
            for (size_t earlier = 0; earlier < i; ++earlier) {
                if (ids[begin + earlier] == ids[begin + i]) { mapped[i] = mapped[earlier]; ++counters.cache_hits; break; }
            }
            if (mapped[i] != slots) { continue; }
            size_t victim = slots;
            uint64_t oldest = UINT64_MAX;
            for (size_t slot = 0; slot < slots; ++slot) {
                if (std::find(reserved.begin(), reserved.end(), slot) != reserved.end()) { continue; }
                if (entries[slot].row == UINT64_MAX) { victim = slot; break; }
                if (entries[slot].age < oldest) { oldest = entries[slot].age; victim = slot; }
            }
            if (victim == slots) { throw std::logic_error("no bounded gather slot"); }
            counters.evictions += entries[victim].row != UINT64_MAX;
            entries[victim] = {UINT64_MAX, ++clock};
            mapped[i] = victim;
            reserved.push_back(victim);
            missing.emplace_back(victim, ids[begin + i]);
        }
        try {
            for (const auto & miss : missing) {
                const auto & span = source(miss.second);
                const uint64_t offset = span.file_offset + (miss.second - span.first_row) * span.row_stride;
                const auto reader = span.reader;
                auto * destination = data.get() + miss.first * row_bytes;
                const size_t length = row_bytes;
                const uint32_t wait = timeout;
                futures.push_back(executor->submit([reader, offset, destination, length, wait] {
                    return reader->read_exact(offset, destination, length, wait);
                }));
            }
            counters.peak_inflight_rows = std::max<uint64_t>(counters.peak_inflight_rows, futures.size());
            bool ok = true;
            for (auto & future : futures) {
                const auto result = future.get();
                ok = ok && result.ok && result.bytes == row_bytes;
                counters.bytes_read += result.bytes;
                counters.rows_read += result.ok;
            }
            if (!ok) { throw std::runtime_error("bounded row read failed; output must be discarded"); }
        } catch (...) {
            for (auto & future : futures) { if (future.valid()) { future.wait(); } }
            throw;
        }
        for (const auto & miss : missing) { entries[miss.first].row = miss.second; }
        for (size_t i = 0; i < n; ++i) {
            std::memcpy(static_cast<uint8_t *>(output) + (begin + i) * row_bytes, data.get() + mapped[i] * row_bytes, row_bytes);
        }
    }
}

row_stream_stats row_stream::snapshot() const { std::lock_guard<std::mutex> lock(mutex); return counters; }
}
