// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright (c) 2026 SparkMoE contributors.

#include "moe-paging.h"

#include "llama-impl.h"

#include <algorithm>
#include <cstdio>
#include <exception>
#include <limits>
#include <stdexcept>

namespace llama_moe {

bool mode_uses_explicit_paging(enum llama_moe_paging_mode mode) {
    return mode == LLAMA_MOE_PAGING_MODE_EXPLICIT;
}

paging_manager::paging_manager(
        ggml_backend_t backend,
        const llama_moe_paging_params & params,
        const model_index & index,
        int64_t minimum_slots,
        uint32_t minimum_ubatch) :
    backend(backend), params(params), index(index), minimum_slots(minimum_slots),
    executor(std::make_shared<io_executor>(params.io_threads > 0 ? params.io_threads : 1)) {
    if (!backend || !mode_uses_explicit_paging(params.mode)) {
        throw std::runtime_error("explicit MoE paging requires a CPU backend");
    }
    if (index.tensor_count() == 0 || index.layer_count() == 0) {
        throw std::runtime_error("the model does not contain a usable MoE paging index");
    }
    if (minimum_slots <= 0 || minimum_ubatch == 0) {
        throw std::runtime_error("invalid active expert count for MoE paging");
    }
    if (params.n_slots <= 0 && params.cache_bytes == 0) {
        throw std::runtime_error("explicit MoE paging requires a slot count or cache byte budget");
    }
    if (params.cache_policy == LLAMA_MOE_CACHE_HYBRID) {
        uint64_t configured = 0;
        for (size_t layer = 0; layer < index.layer_count(); ++layer) {
            if (!index.has_layer(static_cast<int32_t>(layer))) { continue; }
            const uint64_t bytes = index.committed_bytes_per_expert(static_cast<int32_t>(layer));
            minimum_budget += bytes * minimum_slots;
            configured += bytes * std::min<int64_t>(index.expert_count(static_cast<int32_t>(layer)),
                std::max<int64_t>(minimum_slots, params.n_slots));
        }
        budget = std::make_shared<cache_budget>(params.cache_bytes ? params.cache_bytes : configured, minimum_budget, minimum_slots);
        minimum_resize_budget = minimum_budget;
        for (size_t layer = 0; layer < index.layer_count(); ++layer) {
            if (!index.has_layer(static_cast<int32_t>(layer))) { continue; }
            const uint64_t required = uint64_t(minimum_ubatch) * minimum_slots;
            if (required > uint64_t(index.expert_count(static_cast<int32_t>(layer)))) {
                throw std::runtime_error("speculative rollback microbatch exceeds available expert slots");
            }
            minimum_resize_budget = std::max(minimum_resize_budget, minimum_budget +
                (required - minimum_slots) * index.committed_bytes_per_expert(static_cast<int32_t>(layer)));
        }
    }
}

paging_manager::~paging_manager() = default;

int64_t paging_manager::slots_for_layer(int32_t layer) const {
    const int64_t experts = index.expert_count(layer);
    if (experts <= 0) {
        throw std::runtime_error("missing expert metadata for layer " + std::to_string(layer));
    }
    if (params.cache_policy == LLAMA_MOE_CACHE_HYBRID) { return experts; }

    const uint64_t layer_bytes = params.cache_policy == LLAMA_MOE_CACHE_HYBRID ?
        index.committed_bytes_per_expert(layer) : index.bytes_per_expert(layer);
    uint64_t layers_count = 0;
    for (size_t i = 0; i < index.layer_count(); ++i) { layers_count += index.has_layer(static_cast<int32_t>(i)); }
    layers_count = std::max<uint64_t>(1, layers_count);
    int64_t budget_slots = experts;
    if (params.cache_bytes > 0) {
        if (layer_bytes == 0) {
            throw std::runtime_error("invalid zero-sized expert layout");
        }
        budget_slots = static_cast<int64_t>((params.cache_bytes / layers_count) / layer_bytes);
        if (budget_slots < minimum_slots) {
            const uint64_t required = layer_bytes * static_cast<uint64_t>(minimum_slots) * layers_count;
            throw std::runtime_error(
                "MoE cache budget is too small for the active experts; at least " +
                std::to_string(required) + " bytes are required");
        }
    }

    int64_t slots = params.n_slots > 0 ? params.n_slots : budget_slots;
    slots = std::min(slots, budget_slots);
    slots = std::max(slots, minimum_slots);
    return std::min(slots, experts);
}

layer_cache & paging_manager::get_or_create_layer(int32_t layer) {
    std::lock_guard<std::mutex> lock(mutex);
    auto & cache = layers[layer];
    if (!cache) {
        cache = std::make_unique<layer_cache>(
            layer,
            index.expert_count(layer),
            slots_for_layer(layer),
            params.io_threads > 0 ? static_cast<uint32_t>(params.io_threads) : 1,
            params.io_timeout_ms,
            index,
            backend, executor, budget, params.overlap);
        if (params.temporal_prefetch) { cache->after_resolve([this, layer] { prefetch_after(layer); }); }
        LLAMA_LOG_INFO("MoE paging: layer %d uses %lld expert slots\n", layer, static_cast<long long>(cache->slots()));
    }
    return *cache;
}

void paging_manager::prefetch_after(int32_t layer) {
    layer_cache * next = nullptr;
    int32_t next_id = INT32_MAX;
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (const auto & item : layers) {
            if (item.first > layer && item.first < next_id) { next = item.second.get(); next_id = item.first; }
        }
        if (!next) {
            for (const auto & item : layers) {
                if (item.first < next_id) { next = item.second.get(); next_id = item.first; }
            }
        }
    }
    if (next && next_id != layer) { next->prefetch_previous(static_cast<size_t>(std::min<int64_t>(minimum_slots, 16))); }
}

ggml_tensor * paging_manager::bind_pool(int32_t layer, const ggml_tensor * original) {
    if (!original) {
        return nullptr;
    }
    const auto * descriptor = index.find(original->name);
    if (!descriptor) {
        throw std::runtime_error("expert tensor is absent from the paging index: " + std::string(original->name));
    }
    return get_or_create_layer(layer).bind_tensor(original, *descriptor);
}

bool paging_manager::handles_layer(int32_t layer) const {
    return index.has_layer(layer);
}

bool paging_manager::set_expert_pinned(int32_t layer, int32_t expert, bool pinned) {
    if (!index.has_layer(layer)) {
        return false;
    }
    return get_or_create_layer(layer).set_pinned(expert, pinned);
}

uint32_t paging_manager::max_safe_ubatch() const {
    uint64_t result = std::numeric_limits<uint32_t>::max();
    bool found_layer = false;
    for (size_t layer = 0; layer < index.layer_count(); ++layer) {
        if (!index.has_layer(static_cast<int32_t>(layer))) {
            continue;
        }
        found_layer = true;
        int64_t slots = slots_for_layer(static_cast<int32_t>(layer));
        if (budget) {
            std::lock_guard<std::recursive_mutex> lock(budget->mutex);
            const uint64_t bytes = index.committed_bytes_per_expert(static_cast<int32_t>(layer));
            slots = std::min<uint64_t>(index.expert_count(static_cast<int32_t>(layer)),
                minimum_slots + (budget->limit - minimum_budget) / bytes);
        }
        result = std::min(result, static_cast<uint64_t>(slots / minimum_slots));
    }
    if (!found_layer || result == 0) {
        throw std::runtime_error("MoE paging cannot form a safe microbatch with the configured slots");
    }
    return static_cast<uint32_t>(result);
}

void paging_manager::remap_callback(
        ggml_tensor * destination,
        const ggml_tensor * source,
        int thread_index,
        int thread_count,
        void * user_data) {
    GGML_UNUSED(thread_count);
    if (thread_index != 0) {
        return;
    }
    auto * cache = static_cast<layer_cache *>(user_data);
    if (!cache || source->type != GGML_TYPE_I32 || destination->type != GGML_TYPE_I32 ||
            !ggml_is_contiguous(source) || !ggml_is_contiguous(destination)) {
        GGML_ABORT("invalid MoE paging remap operation");
    }
    try {
        cache->resolve(
            static_cast<const int32_t *>(source->data),
            static_cast<size_t>(ggml_nelements(source)),
            static_cast<int32_t *>(destination->data), true);
    } catch (const std::exception & error) {
        std::fprintf(stderr, "MoE paging failed: %s\n", error.what());
        GGML_ABORT("MoE paging I/O or cache failure");
    }
}

ggml_tensor * paging_manager::remap_ids(
        ggml_context * ctx,
        ggml_tensor * selected_experts,
        int32_t layer) {
    auto & cache = get_or_create_layer(layer);
    auto * remapped = ggml_map_custom1(ctx, selected_experts, remap_callback, 1, &cache);
    ggml_set_name(remapped, ("moe_slot_ids_" + std::to_string(layer)).c_str());
    return remapped;
}

stats_snapshot paging_manager::snapshot() const {
    stats_snapshot total;
    std::lock_guard<std::mutex> lock(mutex);
    for (const auto & item : layers) {
        const auto value = item.second->get_stats().snapshot();
        total.cache_hits += value.cache_hits;
        total.cache_misses += value.cache_misses;
        total.bytes_read += value.bytes_read;
        total.read_count += value.read_count;
        total.read_latency_us += value.read_latency_us;
        total.evictions += value.evictions;
        total.waits += value.waits;
        total.duplicate_requests += value.duplicate_requests;
        total.physical_bytes += value.physical_bytes;
        total.direct_bytes += value.direct_bytes;
        total.fallback_reads += value.fallback_reads;
        total.io_queue_us += value.io_queue_us;
        total.remap_wait_us += value.remap_wait_us;
        total.compute_wait_us += value.compute_wait_us;
        total.async_experts += value.async_experts;
        total.compute_with_io_pending += value.compute_with_io_pending;
        total.prefetch_reads += value.prefetch_reads;
        total.prefetch_hits += value.prefetch_hits;
        total.prefetch_wasted += value.prefetch_wasted;
        total.prefetch_promoted += value.prefetch_promoted;
        total.prefetch_skipped += value.prefetch_skipped;
        total.cache_committed_bytes += item.second->committed_bytes();
        total.resident_experts += item.second->resident_count();
        total.pinned_experts += item.second->pinned_count();
    }
    if (budget) {
        std::lock_guard<std::recursive_mutex> cache_lock(budget->mutex);
        total.cache_limit_bytes = budget->limit;
    } else { total.cache_limit_bytes = params.cache_bytes ? params.cache_bytes : total.cache_committed_bytes; }
    total.resize_pending_bytes = pending_resize.load();
    total.resize_failures = resize_failures.load();
    return total;
}

bool paging_manager::request_resize(uint64_t bytes) {
    if (!budget || bytes < minimum_resize_budget) { return false; }
    pending_resize.store(bytes);
    return true;
}

bool paging_manager::has_pending_resize() const { return pending_resize.load() != 0; }

void paging_manager::apply_pending_resize() {
    const uint64_t bytes = pending_resize.exchange(0);
    if (!bytes || !budget) { return; }
    budget->release_active();
    if (!budget->resize(bytes)) {
        ++resize_failures;
        LLAMA_LOG_WARN("MoE paging: resize rejected by pinned/active expert or per-layer floor constraints\n");
    }
}

void paging_manager::reset_stats() {
    resize_failures.store(0);
    std::lock_guard<std::mutex> lock(mutex);
    for (auto & item : layers) {
        item.second->reset_stats();
    }
}

}
