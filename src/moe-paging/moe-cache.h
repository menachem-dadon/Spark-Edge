// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright (c) 2026 SparkMoE contributors.

#pragma once

#include "ggml-backend.h"
#include "ggml-cpp.h"
#include "moe-io.h"
#include "moe-model-index.h"
#include "moe-stats.h"
#include "moe-memory.h"
#include "moe-readiness.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace llama_moe {

class layer_cache;
class cache_budget {
public:
    cache_budget(uint64_t limit, uint64_t minimum, int64_t floor);
    bool resize(uint64_t bytes);
    void release_active();
    uint64_t limit;
    uint64_t committed = 0;
    uint64_t clock = 0;
    std::recursive_mutex mutex;
private:
    friend class layer_cache;
    void make_room(layer_cache * recipient, uint64_t bytes);
    std::pair<layer_cache *, int32_t> oldest(layer_cache * recipient) const;
    uint64_t minimum;
    int64_t floor;
    layer_cache * active = nullptr;
    std::vector<layer_cache *> layers;
    bool reclaim_pending(layer_cache * recipient);
};

enum class entry_state {
    empty,
    loading,
    ready,
    in_use,
    error,
};

struct cache_entry {
    int32_t expert_id = -1;
    int32_t slot_id = -1;
    entry_state state = entry_state::empty;
    uint64_t last_use = 0;
    bool pinned = false;
    uint32_t active_refs = 0;
    uint64_t generation = 0;
    bool committed = false;
    bool prefetched = false;
};

struct tensor_pool {
    ggml_tensor * tensor = nullptr;
    ggml_context_ptr context;
    std::unique_ptr<virtual_memory> memory;
    ggml_backend_buffer_ptr buffer;
    const tensor_descriptor * descriptor = nullptr;
    std::shared_ptr<readiness_table> readiness;
    ggml_context_ptr device_context;
    ggml_backend_buffer_ptr device_buffer;
    ggml_tensor * device_tensor = nullptr;
    std::vector<uint64_t> device_generations;
};

class layer_cache {
public:
    layer_cache(
        int32_t layer,
        int64_t expert_count,
        int64_t slot_count,
        uint32_t io_threads,
        uint32_t io_timeout_ms,
        const model_index & index,
        ggml_backend_t backend,
        std::shared_ptr<io_executor> executor = nullptr,
        std::shared_ptr<cache_budget> budget = nullptr,
        bool overlap = false);
    ~layer_cache();

    ggml_tensor * bind_tensor(const ggml_tensor * original, const tensor_descriptor & descriptor);
    ggml_tensor * bind_device(const ggml_tensor * host, ggml_backend_t device, uint64_t & bytes_left);
    void resolve(const int32_t * expert_ids, size_t count, int32_t * slot_ids, bool graph = false);
    bool set_pinned(int32_t expert, bool pinned);

    int64_t slots() const;
    const stats & get_stats() const;
    void reset_stats();
    uint64_t resident_count() const;
    uint64_t pinned_count() const;
    uint64_t committed_bytes() const;
    uint64_t device_committed_bytes() const;
    void prefetch_previous(size_t maximum);
    void after_resolve(std::function<void()> callback) { next_layer_prefetch = std::move(callback); }

private:
    friend class cache_budget;
    std::recursive_mutex & cache_mutex() const;
    void evict(int32_t slot);
    void commit_slot(int32_t slot);
    void release_active();
    bool finish_slot(int32_t slot, bool required);
    void refresh_completed();
    void queue_slot(int32_t expert, int32_t slot, bool speculative);
    int32_t choose_victim(const std::vector<bool> & reserved) const;

    int32_t layer;
    int64_t n_experts;
    int64_t n_slots;
    uint32_t io_timeout_ms;
    const model_index & index;
    ggml_backend_t backend;
    std::shared_ptr<io_executor> executor;
    std::shared_ptr<cache_budget> budget;
    uint64_t expert_bytes = 0;

    mutable std::recursive_mutex mutex;
    uint64_t clock = 0;
    std::vector<cache_entry> entries;
    std::vector<int32_t> expert_to_slot;
    std::vector<bool> pin_requested;
    std::vector<tensor_pool> pools;
    std::unordered_map<std::string, size_t> pool_by_name;
    std::shared_ptr<stats> counters = std::make_shared<stats>();
    bool overlap;
    ggml_backend_t device = nullptr;
    std::vector<int32_t> previous_experts;
    std::function<void()> next_layer_prefetch;
};

}
