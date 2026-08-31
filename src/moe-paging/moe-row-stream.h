// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright (c) 2026 SparkMoE contributors.
#pragma once
#include "moe-io.h"
#include <mutex>
#include <vector>

namespace llama_moe {
struct row_span {
    uint64_t first_row;
    uint64_t row_count;
    uint64_t file_offset;
    uint64_t row_stride;
    std::shared_ptr<file_reader> reader;
};

struct row_stream_stats {
    uint64_t allocated_bytes = 0;
    uint64_t cache_hits = 0;
    uint64_t rows_read = 0;
    uint64_t bytes_read = 0;
    uint64_t evictions = 0;
    uint64_t peak_inflight_rows = 0;
};

// Byte-exact gather only. The caller owns the output; there is no full-table fallback.
class row_stream {
public:
    row_stream(std::vector<row_span> spans, size_t row_bytes, uint64_t budget,
               size_t max_inflight_rows, std::shared_ptr<io_executor> executor, uint32_t timeout_ms = 30000);
    void copy_rows(const uint64_t * ids, size_t count, void * output, size_t output_bytes);
    row_stream_stats snapshot() const;
    uint64_t row_count() const { return rows; }
private:
    struct entry { uint64_t row = UINT64_MAX; uint64_t age = 0; };
    const row_span & source(uint64_t row) const;
    std::vector<row_span> spans;
    size_t row_bytes;
    size_t slots;
    size_t max_inflight;
    uint32_t timeout;
    uint64_t rows = 0, clock = 0;
    std::shared_ptr<io_executor> executor;
    std::unique_ptr<entry[]> entries;
    std::unique_ptr<uint8_t[]> data;
    mutable std::mutex mutex;
    row_stream_stats counters;
};
}
