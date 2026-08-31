// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright (c) 2026 SparkMoE contributors.

#include "moe-stats.h"

namespace llama_moe {

void stats::add_hit(uint64_t n) { cache_hits.fetch_add(n, std::memory_order_relaxed); }
void stats::add_miss(uint64_t n) { cache_misses.fetch_add(n, std::memory_order_relaxed); }
void stats::add_eviction(uint64_t n) { evictions.fetch_add(n, std::memory_order_relaxed); }
void stats::add_wait(uint64_t n) { waits.fetch_add(n, std::memory_order_relaxed); }
void stats::add_duplicate(uint64_t n) { duplicate_requests.fetch_add(n, std::memory_order_relaxed); }
void stats::add(metric field, uint64_t n) { extra[static_cast<size_t>(field)].fetch_add(n, std::memory_order_relaxed); }

void stats::add_read(uint64_t bytes, uint64_t latency_us, uint64_t physical, uint64_t direct, uint64_t fallback) {
    physical_bytes.fetch_add(physical, std::memory_order_relaxed);
    direct_bytes.fetch_add(direct, std::memory_order_relaxed);
    fallback_reads.fetch_add(fallback, std::memory_order_relaxed);
    bytes_read.fetch_add(bytes, std::memory_order_relaxed);
    read_count.fetch_add(1, std::memory_order_relaxed);
    read_latency_us.fetch_add(latency_us, std::memory_order_relaxed);
}

stats_snapshot stats::snapshot() const {
    stats_snapshot result{
        cache_hits.load(std::memory_order_relaxed),
        cache_misses.load(std::memory_order_relaxed),
        bytes_read.load(std::memory_order_relaxed),
        read_count.load(std::memory_order_relaxed),
        read_latency_us.load(std::memory_order_relaxed),
        evictions.load(std::memory_order_relaxed),
        waits.load(std::memory_order_relaxed),
        duplicate_requests.load(std::memory_order_relaxed),
        physical_bytes.load(std::memory_order_relaxed),
        direct_bytes.load(std::memory_order_relaxed),
        fallback_reads.load(std::memory_order_relaxed),
    };
    result.io_queue_us = extra[static_cast<size_t>(metric::io_queue_us)].load();
    result.remap_wait_us = extra[static_cast<size_t>(metric::remap_wait_us)].load();
    result.compute_wait_us = extra[static_cast<size_t>(metric::compute_wait_us)].load();
    result.async_experts = extra[static_cast<size_t>(metric::async_experts)].load();
    result.compute_with_io_pending = extra[static_cast<size_t>(metric::compute_with_io_pending)].load();
    result.prefetch_reads = extra[static_cast<size_t>(metric::prefetch_reads)].load();
    result.prefetch_hits = extra[static_cast<size_t>(metric::prefetch_hits)].load();
    result.prefetch_wasted = extra[static_cast<size_t>(metric::prefetch_wasted)].load();
    result.prefetch_promoted = extra[static_cast<size_t>(metric::prefetch_promoted)].load();
    result.prefetch_skipped = extra[static_cast<size_t>(metric::prefetch_skipped)].load();
    return result;
}

void stats::reset() {
    for (auto & value : extra) { value.store(0); }
    cache_hits.store(0, std::memory_order_relaxed);
    cache_misses.store(0, std::memory_order_relaxed);
    bytes_read.store(0, std::memory_order_relaxed);
    read_count.store(0, std::memory_order_relaxed);
    read_latency_us.store(0, std::memory_order_relaxed);
    evictions.store(0, std::memory_order_relaxed);
    waits.store(0, std::memory_order_relaxed);
    duplicate_requests.store(0, std::memory_order_relaxed);
    physical_bytes.store(0, std::memory_order_relaxed);
    direct_bytes.store(0, std::memory_order_relaxed);
    fallback_reads.store(0, std::memory_order_relaxed);
}

}
