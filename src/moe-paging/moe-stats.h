// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright (c) 2026 SparkMoE contributors.

#pragma once

#include <atomic>
#include <cstdint>
#include <array>
#include <cstddef>

namespace llama_moe {

enum class metric { io_queue_us, remap_wait_us, compute_wait_us, async_experts,
    compute_with_io_pending, prefetch_reads, prefetch_hits, prefetch_wasted,
    prefetch_promoted, prefetch_skipped, count };

struct stats_snapshot {
    uint64_t cache_hits = 0;
    uint64_t cache_misses = 0;
    uint64_t bytes_read = 0;
    uint64_t read_count = 0;
    uint64_t read_latency_us = 0;
    uint64_t evictions = 0;
    uint64_t waits = 0;
    uint64_t duplicate_requests = 0;
    uint64_t physical_bytes = 0;
    uint64_t direct_bytes = 0;
    uint64_t fallback_reads = 0;
    uint64_t cache_committed_bytes = 0;
    uint64_t cache_limit_bytes = 0;
    uint64_t resident_experts = 0;
    uint64_t pinned_experts = 0;
    uint64_t resize_pending_bytes = 0;
    uint64_t resize_failures = 0;
    uint64_t io_queue_us = 0;
    uint64_t remap_wait_us = 0;
    uint64_t compute_wait_us = 0;
    uint64_t async_experts = 0;
    uint64_t compute_with_io_pending = 0;
    uint64_t prefetch_reads = 0;
    uint64_t prefetch_hits = 0;
    uint64_t prefetch_wasted = 0;
    uint64_t prefetch_promoted = 0;
    uint64_t prefetch_skipped = 0;
};

class stats {
public:
    void add_hit(uint64_t n = 1);
    void add_miss(uint64_t n = 1);
    void add_read(uint64_t bytes, uint64_t latency_us, uint64_t physical = 0, uint64_t direct = 0, uint64_t fallback = 0);
    void add_eviction(uint64_t n = 1);
    void add_wait(uint64_t n = 1);
    void add_duplicate(uint64_t n = 1);
    void add(metric field, uint64_t n = 1);

    stats_snapshot snapshot() const;
    void reset();

private:
    std::array<std::atomic<uint64_t>, static_cast<size_t>(metric::count)> extra{};
    std::atomic<uint64_t> cache_hits{0};
    std::atomic<uint64_t> cache_misses{0};
    std::atomic<uint64_t> bytes_read{0};
    std::atomic<uint64_t> read_count{0};
    std::atomic<uint64_t> read_latency_us{0};
    std::atomic<uint64_t> evictions{0};
    std::atomic<uint64_t> waits{0};
    std::atomic<uint64_t> duplicate_requests{0};
    std::atomic<uint64_t> physical_bytes{0};
    std::atomic<uint64_t> direct_bytes{0};
    std::atomic<uint64_t> fallback_reads{0};
};

}
