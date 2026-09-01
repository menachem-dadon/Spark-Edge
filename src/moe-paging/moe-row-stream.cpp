// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright (c) 2026 SparkMoE contributors.
#include "moe-row-stream.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace llama_moe {
row_stream::row_stream(std::vector<row_span> spans, size_t row_bytes, uint64_t budget,
                       size_t max_inflight_rows, std::shared_ptr<io_executor> executor, uint32_t timeout_ms, size_t page_bytes) :
    spans(std::move(spans)), row_bytes(row_bytes), max_inflight(max_inflight_rows), timeout(timeout_ms), executor(std::move(executor)) {
    if (!row_bytes || row_bytes > SIZE_MAX - sizeof(entry) - 2 * sizeof(size_t) || !max_inflight || max_inflight > 32 || !this->executor ||
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
    if (page_bytes) {
        if (page_bytes < row_bytes || page_bytes > 65536 || (page_bytes & (page_bytes - 1))) {
            throw std::invalid_argument("invalid row cache page size");
        }
        std::vector<row_span> page_spans;
        uint64_t first = 0;
        for (const auto & span : this->spans) {
            const uint64_t n = span.reader->size() / page_bytes;
            if (!n || n > UINT64_MAX - first) { throw std::invalid_argument("invalid page span"); }
            page_bases.push_back(first);
            page_spans.push_back({first, n, 0, page_bytes, span.reader});
            first += n;
        }
        counters.allocated_bytes = metadata_bytes + page_bases.capacity() * sizeof(uint64_t);
        if (budget <= counters.allocated_bytes) { throw std::invalid_argument("page metadata exceeds budget"); }
        pages = std::make_unique<row_stream>(std::move(page_spans), page_bytes, budget - counters.allocated_bytes,
                max_inflight, this->executor, timeout);
        this->page_bytes = page_bytes;
        return;
    }
    const uint64_t capacity = std::min<uint64_t>({65536, rows, (budget - metadata_bytes) / (row_bytes + sizeof(entry) + 2 * sizeof(size_t))});
    if (!capacity || capacity > SIZE_MAX / row_bytes) { throw std::invalid_argument("row budget cannot hold one row"); }
    slots = static_cast<size_t>(capacity);
    max_inflight = std::min(max_inflight, slots);
    entries = std::make_unique<entry[]>(slots);
    buckets = std::make_unique<size_t[]>(2 * slots);
    std::fill_n(buckets.get(), 2 * slots, SIZE_MAX);
    for (size_t slot = 0; slot < slots; ++slot) {
        entries[slot].prev = slot ? slot - 1 : SIZE_MAX;
        entries[slot].next = slot + 1 < slots ? slot + 1 : SIZE_MAX;
    }
    newest = 0;
    oldest = slots - 1;
    data = std::make_unique<uint8_t[]>(slots * row_bytes);
    counters.allocated_bytes = metadata_bytes + slots * (row_bytes + sizeof(entry) + 2 * sizeof(size_t));
}

size_t row_stream::find(uint64_t row) const {
    size_t slot = buckets[(row * 11400714819323198485ull) % (2 * slots)];
    while (slot != SIZE_MAX && entries[slot].row != row) { slot = entries[slot].hash_next; }
    return slot;
}

void row_stream::erase(size_t slot) {
    auto & e = entries[slot];
    if (e.row != UINT64_MAX) {
        auto * link = &buckets[(e.row * 11400714819323198485ull) % (2 * slots)];
        while (*link != slot) { link = &entries[*link].hash_next; }
        *link = e.hash_next;
        e.row = UINT64_MAX;
        e.hash_next = SIZE_MAX;
    }
}

void row_stream::publish(size_t slot, uint64_t row) {
    auto & bucket = buckets[(row * 11400714819323198485ull) % (2 * slots)];
    entries[slot].row = row;
    entries[slot].hash_next = bucket;
    bucket = slot;
}

void row_stream::touch(size_t slot) {
    if (slot == newest) { return; }
    auto & e = entries[slot];
    if (e.prev != SIZE_MAX) { entries[e.prev].next = e.next; }
    if (e.next != SIZE_MAX) { entries[e.next].prev = e.prev; } else { oldest = e.prev; }
    e.prev = SIZE_MAX;
    e.next = newest;
    entries[newest].prev = slot;
    newest = slot;
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
    if (pages) { copy_page_rows(ids, count, static_cast<uint8_t *>(output)); return; }
    for (size_t begin = 0; begin < count; begin += max_inflight) {
        const size_t n = std::min(max_inflight, count - begin);
        std::vector<size_t> mapped(n, slots), reserved;
        std::vector<std::pair<size_t, uint64_t>> missing;
        std::vector<std::future<io_result>> futures;
        reserved.reserve(n); missing.reserve(n); futures.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            const size_t slot = find(ids[begin + i]);
            if (slot != SIZE_MAX) {
                mapped[i] = slot; reserved.push_back(slot); ++counters.cache_hits;
            }
        }
        for (size_t i = 0; i < n; ++i) {
            if (mapped[i] != slots) { touch(mapped[i]); continue; }
            for (size_t earlier = 0; earlier < i; ++earlier) {
                if (ids[begin + earlier] == ids[begin + i]) { mapped[i] = mapped[earlier]; ++counters.cache_hits; break; }
            }
            if (mapped[i] != slots) { continue; }
            size_t victim = oldest;
            while (victim != SIZE_MAX && std::find(reserved.begin(), reserved.end(), victim) != reserved.end()) {
                victim = entries[victim].prev;
            }
            if (victim == SIZE_MAX) { throw std::logic_error("no bounded gather slot"); }
            counters.evictions += entries[victim].row != UINT64_MAX;
            erase(victim);
            touch(victim);
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
        for (const auto & miss : missing) { publish(miss.first, miss.second); }
        for (size_t i = 0; i < n; ++i) {
            std::memcpy(static_cast<uint8_t *>(output) + (begin + i) * row_bytes, data.get() + mapped[i] * row_bytes, row_bytes);
        }
    }
}

void row_stream::copy_page_rows(const uint64_t * ids, size_t count, uint8_t * output) {
    struct piece { uint64_t page; size_t offset, length, destination; };
    std::vector<piece> pieces;
    std::vector<uint64_t> ids_pages;
    for (size_t i = 0; i < count; ++i) {
        const auto & span = source(ids[i]);
        const size_t source_index = &span - spans.data();
        const uint64_t start = span.file_offset + (ids[i] - span.first_row) * span.row_stride;
        for (size_t copied = 0; copied < row_bytes;) {
            const uint64_t position = start + copied;
            const uint64_t page = position / page_bytes;
            const size_t offset = size_t(position % page_bytes);
            const size_t length = std::min(row_bytes - copied, page_bytes - offset);
            if (page == span.reader->size() / page_bytes) {
                const auto part = span.reader->read_exact(position, output + i * row_bytes + copied, length, timeout);
                if (!part.ok || part.bytes != length) { throw std::runtime_error("PLE shard tail read failed"); }
                counters.bytes_read += part.bytes;
            } else {
                const uint64_t id = page_bases[source_index] + page;
                pieces.push_back({id, offset, length, i * row_bytes + copied});
                ids_pages.push_back(id);
            }
            copied += length;
        }
    }
    std::sort(ids_pages.begin(), ids_pages.end());
    ids_pages.erase(std::unique(ids_pages.begin(), ids_pages.end()), ids_pages.end());
    std::vector<uint8_t> scratch(std::min<size_t>(256, ids_pages.size()) * page_bytes);
    for (size_t begin = 0; begin < ids_pages.size(); begin += 256) {
        const size_t n = std::min<size_t>(256, ids_pages.size() - begin);
        pages->copy_rows(ids_pages.data() + begin, n, scratch.data(), scratch.size());
        for (const auto & part : pieces) {
            if (part.page < ids_pages[begin] || part.page > ids_pages[begin + n - 1]) { continue; }
            const size_t index = std::lower_bound(ids_pages.begin() + begin, ids_pages.begin() + begin + n, part.page) - ids_pages.begin() - begin;
            std::memcpy(output + part.destination, scratch.data() + index * page_bytes + part.offset, part.length);
        }
    }
}

row_stream_stats row_stream::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex);
    if (!pages) { return counters; }
    auto result = pages->snapshot();
    result.allocated_bytes += counters.allocated_bytes;
    result.bytes_read += counters.bytes_read;
    return result;
}

void row_stream::reset_stats() {
    std::lock_guard<std::mutex> lock(mutex);
    if (pages) { pages->reset_stats(); }
    const uint64_t allocated = counters.allocated_bytes;
    counters = {};
    counters.allocated_bytes = allocated;
}

void row_stream::copy_rows_f32(const int32_t * ids, size_t count, ggml_type type, int64_t width, float * output) {
    if (type < 0 || type >= GGML_TYPE_COUNT) { throw std::invalid_argument("invalid row type"); }
    const auto * traits = ggml_get_type_traits(type);
    if (!ids || !output || !count || width <= 0 || (!traits->to_float && type != GGML_TYPE_F32) ||
            width % ggml_blck_size(type) || ggml_row_size(type, width) != row_bytes ||
            count > SIZE_MAX / sizeof(float) / size_t(width)) {
        throw std::invalid_argument("invalid quantized row gather");
    }
    for (size_t i = 0; i < count; ++i) {
        if (ids[i] < 0 || uint64_t(ids[i]) >= rows) { throw std::out_of_range("PLE row is outside the table"); }
    }
    std::vector<uint64_t> chunk_ids(std::min<size_t>(count, 256));
    std::vector<uint8_t> raw(chunk_ids.size() * row_bytes);
    for (size_t begin = 0; begin < count; begin += chunk_ids.size()) {
        const size_t n = std::min(chunk_ids.size(), count - begin);
        for (size_t i = 0; i < n; ++i) { chunk_ids[i] = uint64_t(ids[begin + i]); }
        copy_rows(chunk_ids.data(), n, raw.data(), raw.size());
        for (size_t i = 0; i < n; ++i) {
            if (type == GGML_TYPE_F32) {
                std::memcpy(output + (begin + i) * width, raw.data() + i * row_bytes, row_bytes);
            } else {
                traits->to_float(raw.data() + i * row_bytes, output + (begin + i) * width, width);
            }
        }
    }
}
}
