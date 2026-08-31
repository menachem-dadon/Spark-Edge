// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright (c) 2026 SparkMoE contributors.
#include "moe-readiness.h"
#include "ggml-cpu.h"
#include <chrono>
#include <cstdio>
#include <unordered_map>

namespace llama_moe {
static std::mutex registry_mutex;
static std::unordered_map<const ggml_tensor *, std::shared_ptr<readiness_table>> registry;
static std::atomic<uint64_t> next_tag{1};

read_ticket::read_ticket(std::shared_ptr<io_executor> executor, std::shared_ptr<stats> counters,
                        std::function<io_result()> read, bool speculative) :
    executor(std::move(executor)), counters(std::move(counters)), read(std::move(read)),
    tag(next_tag.fetch_add(1)), speculative(speculative) {
    future = this->executor->submit(this->read, speculative ? io_priority::prefetch : io_priority::demand, tag).share();
}

bool read_ticket::ready() const { return future.wait_for(std::chrono::seconds(0)) == std::future_status::ready; }
read_ticket::~read_ticket() {
    try { cancel(); finish(false); } catch (...) {}
}
void read_ticket::promote() { if (executor->promote(tag)) { counters->add(metric::prefetch_promoted); } }
void read_ticket::cancel() { executor->cancel(tag); }

const io_result & read_ticket::finish(bool required, bool compute) {
    const auto start = std::chrono::steady_clock::now();
    std::call_once(collect_once, [&] {
        try { result = future.get(); }
        catch (const std::exception & error) { result.error = error.what(); }
        if (result.ok) {
            counters->add_read(result.bytes, result.latency_us, result.physical_bytes, result.direct_bytes, result.fallback_reads);
            counters->add(metric::io_queue_us, result.queue_us);
            if (speculative) { counters->add(metric::prefetch_reads); }
        }
    });
    if (required && speculative) {
        std::call_once(retry_once, [&] {
            if (!result.ok) {
                retried = true;
                try { retry_result = read(); }
                catch (const std::exception & error) { retry_result.error = error.what(); }
                if (retry_result.ok) {
                    counters->add_read(retry_result.bytes, retry_result.latency_us, retry_result.physical_bytes,
                        retry_result.direct_bytes, retry_result.fallback_reads);
                }
            }
        });
    }
    if (compute) {
        std::call_once(compute_once, [&] {
            counters->add(metric::compute_wait_us, static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - start).count()));
        });
        if (batch && !batch->observed.exchange(true)) {
            for (const auto & weak : batch->reads) {
                const auto other = weak.lock();
                if (other && !other->ready()) { counters->add(metric::compute_with_io_pending); break; }
            }
        }
    }
    return required && speculative && retried ? retry_result : result;
}

static bool wait_tensor(const ggml_tensor * tensor, int64_t slot) {
    std::shared_ptr<readiness_table> table;
    {
        std::lock_guard<std::mutex> lock(registry_mutex);
        const auto found = registry.find(tensor);
        if (found == registry.end()) { return true; }
        table = found->second;
    }
    std::shared_ptr<read_ticket> ticket;
    {
        std::lock_guard<std::mutex> lock(table->mutex);
        if (slot < 0 || static_cast<size_t>(slot) >= table->reads.size()) { return false; }
        ticket = table->reads[static_cast<size_t>(slot)];
    }
    if (!ticket) { return false; }
    try {
        const auto & result = ticket->finish(true, true);
        if (!result.ok) { std::fprintf(stderr, "MoE demand read failed: %s\n", result.error.c_str()); }
        return result.ok;
    } catch (const std::exception & error) {
        std::fprintf(stderr, "MoE demand read exception: %s\n", error.what());
        return false;
    }
}

void register_readiness(const ggml_tensor * tensor, std::shared_ptr<readiness_table> table) {
    std::lock_guard<std::mutex> lock(registry_mutex);
    registry[tensor] = std::move(table);
    ggml_cpu_set_moe_wait(wait_tensor);
}

void unregister_readiness(const ggml_tensor * tensor) {
    std::lock_guard<std::mutex> lock(registry_mutex);
    registry.erase(tensor);
    if (registry.empty()) { ggml_cpu_set_moe_wait(nullptr); }
}
}
