// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright (c) 2026 SparkMoE contributors.

#include "moe-cache.h"

#include <algorithm>
#include <chrono>
#include <future>
#include <stdexcept>
#include <unordered_map>
#include "ggml-alloc.h"
#include "ggml-cpu.h"

namespace llama_moe {

cache_budget::cache_budget(uint64_t limit, uint64_t minimum, int64_t floor) :
    limit(limit), minimum(minimum), floor(floor) {
    if (!limit || limit < minimum || floor <= 0) { throw std::invalid_argument("hybrid cache budget is below per-layer floors"); }
}

std::pair<layer_cache *, int32_t> cache_budget::oldest(layer_cache * recipient) const {
    std::pair<layer_cache *, int32_t> result{nullptr, -1};
    uint64_t age = UINT64_MAX;
    for (auto * cache : layers) {
        cache->refresh_completed();
        if (cache == recipient && cache->resident_count() <= static_cast<uint64_t>(floor)) { continue; }
        for (const auto & entry : cache->entries) {
            if (!entry.committed || entry.pinned || entry.active_refs || entry.state == entry_state::loading) { continue; }
            if (entry.last_use < age) { result = {cache, entry.slot_id}; age = entry.last_use; }
        }
    }
    return result;
}

void cache_budget::make_room(layer_cache * recipient, uint64_t bytes) {
    if (bytes > limit) { throw std::runtime_error("an expert exceeds the hybrid cache budget"); }
    while (committed > limit - bytes) {
        const auto victim = oldest(recipient);
        if (!victim.first) {
            if (reclaim_pending(recipient)) { continue; }
            throw std::runtime_error("hybrid budget exhausted by active or pinned experts");
        }
        victim.first->evict(victim.second);
    }
}

bool cache_budget::reclaim_pending(layer_cache * recipient) {
    for (auto * cache : layers) {
        if (cache == recipient && cache->resident_count() <= static_cast<uint64_t>(floor)) { continue; }
        for (auto & entry : cache->entries) {
            if (entry.prefetched && !entry.active_refs && !entry.pinned) { cache->evict(entry.slot_id); return true; }
        }
    }
    return false;
}

void cache_budget::release_active() {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (active) { active->release_active(); active = nullptr; }
}

bool cache_budget::resize(uint64_t bytes) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (bytes < minimum) { return false; }
    uint64_t reclaimable = 0;
    for (auto * cache : layers) {
        uint64_t count = 0;
        for (const auto & entry : cache->entries) {
            count += entry.committed && !entry.pinned && !entry.active_refs &&
                (entry.state != entry_state::loading || entry.prefetched);
        }
        reclaimable += count * cache->expert_bytes;
    }
    if (committed > bytes && committed - bytes > reclaimable) { return false; }
    while (committed > bytes) {
        const auto victim = oldest(nullptr);
        if (!victim.first) {
            if (reclaim_pending(nullptr)) { continue; }
            return false;
        }
        victim.first->evict(victim.second);
    }
    limit = bytes;
    return true;
}

layer_cache::layer_cache(
        int32_t layer,
        int64_t expert_count,
        int64_t slot_count,
        uint32_t io_threads,
        uint32_t io_timeout_ms,
        const model_index & index,
        ggml_backend_t backend,
        std::shared_ptr<io_executor> executor,
        std::shared_ptr<cache_budget> budget,
        bool overlap) :
    layer(layer),
    n_experts(expert_count),
    n_slots(budget ? expert_count : slot_count),
    io_timeout_ms(io_timeout_ms),
    index(index),
    backend(backend),
    executor(executor ? std::move(executor) : std::make_shared<io_executor>(io_threads)),
    budget(std::move(budget)),
    expert_bytes(index.committed_bytes_per_expert(layer)), overlap(overlap) {
    if (n_experts <= 0 || n_slots <= 0 || n_slots > n_experts) {
        throw std::runtime_error("invalid MoE cache dimensions");
    }
    entries.resize(static_cast<size_t>(n_slots));
    expert_to_slot.assign(static_cast<size_t>(n_experts), -1);
    pin_requested.assign(static_cast<size_t>(n_experts), false);
    for (int32_t slot = 0; slot < n_slots; ++slot) {
        entries[slot].slot_id = slot;
    }
    if (this->budget) {
        std::lock_guard<std::recursive_mutex> lock(this->budget->mutex);
        this->budget->layers.push_back(this);
    }
}

layer_cache::~layer_cache() {
    for (auto & pool : pools) { if (overlap) { unregister_readiness(pool.tensor); } }
    for (auto & entry : entries) {
        if (entry.committed) {
            for (auto & pool : pools) {
                const auto & read = pool.readiness->reads[entry.slot_id];
                if (read) { read->cancel(); read->finish(false); }
            }
        }
    }
    if (budget) {
        std::lock_guard<std::recursive_mutex> lock(budget->mutex);
        if (budget->active == this) { budget->active = nullptr; }
        budget->committed -= committed_bytes();
        auto & list = budget->layers;
        list.erase(std::remove(list.begin(), list.end(), this), list.end());
    }
}

std::recursive_mutex & layer_cache::cache_mutex() const { return budget ? budget->mutex : mutex; }
void layer_cache::release_active() {
    for (auto & entry : entries) {
        if (entry.active_refs) { finish_slot(entry.slot_id, false); }
        entry.active_refs = 0;
    }
}

uint64_t layer_cache::resident_count() const {
    std::lock_guard<std::recursive_mutex> lock(cache_mutex());
    return static_cast<uint64_t>(std::count_if(entries.begin(), entries.end(), [](const cache_entry & e) { return e.committed; }));
}

uint64_t layer_cache::pinned_count() const {
    std::lock_guard<std::recursive_mutex> lock(cache_mutex());
    return static_cast<uint64_t>(std::count(pin_requested.begin(), pin_requested.end(), true));
}

uint64_t layer_cache::committed_bytes() const {
    std::lock_guard<std::recursive_mutex> lock(cache_mutex());
    if (budget) { return resident_count() * expert_bytes; }
    uint64_t bytes = 0;
    for (const auto & pool : pools) { bytes += ggml_backend_buffer_get_size(pool.buffer.get()); }
    return bytes;
}

void layer_cache::evict(int32_t slot) {
    auto & entry = entries[slot];
    for (auto & pool : pools) {
        const auto & read = pool.readiness->reads[slot];
        if (read) { read->cancel(); }
    }
    for (auto & pool : pools) {
        std::lock_guard<std::mutex> ready_lock(pool.readiness->mutex);
        auto & read = pool.readiness->reads[slot];
        if (read) { read->cancel(); read->finish(false); read.reset(); }
    }
    if (entry.prefetched) { counters->add(metric::prefetch_wasted); }
    if (entry.expert_id >= 0) { expert_to_slot[entry.expert_id] = -1; counters->add_eviction(); }
    if (budget && entry.committed) {
        for (auto & pool : pools) { pool.memory->discard(static_cast<uint64_t>(slot) * pool.tensor->nb[2], pool.tensor->nb[2]); }
        budget->committed -= expert_bytes;
    }
    entry = {};
    entry.slot_id = slot;
}

void layer_cache::commit_slot(int32_t slot) {
    if (budget) {
        budget->make_room(this, expert_bytes);
        size_t committed_pools = 0;
        try {
            for (auto & pool : pools) {
                pool.memory->commit(static_cast<uint64_t>(slot) * pool.tensor->nb[2], pool.tensor->nb[2]);
                ++committed_pools;
            }
        } catch (...) {
            for (size_t i = 0; i < committed_pools; ++i) {
                auto & pool = pools[i];
                pool.memory->discard(static_cast<uint64_t>(slot) * pool.tensor->nb[2], pool.tensor->nb[2]);
            }
            throw;
        }
        budget->committed += expert_bytes;
    }
    entries[slot].committed = true;
}

ggml_tensor * layer_cache::bind_tensor(const ggml_tensor * original, const tensor_descriptor & descriptor) {
    std::lock_guard<std::recursive_mutex> lock(cache_mutex());
    const auto existing = pool_by_name.find(descriptor.name);
    if (existing != pool_by_name.end()) {
        return pools[existing->second].tensor;
    }
    if (!original || descriptor.layer != layer || descriptor.expert_count != n_experts) {
        throw std::runtime_error("expert tensor does not match its paging layer: " + descriptor.name);
    }

    tensor_pool pool;
    ggml_init_params params{};
    params.mem_size = ggml_tensor_overhead() * 2;
    params.no_alloc = true;
    pool.context.reset(ggml_init(params));
    if (!pool.context) {
        throw std::runtime_error("failed to allocate MoE tensor metadata");
    }

    pool.tensor = ggml_new_tensor_3d(
        pool.context.get(), original->type, original->ne[0], original->ne[1], n_slots);
    ggml_set_name(pool.tensor, (descriptor.name + "_slots").c_str());
    if (pool.tensor->nb[2] != descriptor.expert_stride) {
        throw std::runtime_error("slot layout differs from GGUF expert layout: " + descriptor.name);
    }

    if (budget) {
        if (resident_count()) { throw std::runtime_error("cannot add a tensor to an active hybrid layer"); }
        pool.tensor->nb[2] = static_cast<size_t>(page_aligned(descriptor.expert_stride));
        if (static_cast<uint64_t>(n_slots) > SIZE_MAX / pool.tensor->nb[2]) { throw std::overflow_error("virtual expert pool overflow"); }
        pool.tensor->nb[3] = pool.tensor->nb[2] * n_slots;
        pool.memory = std::make_unique<virtual_memory>(pool.tensor->nb[3]);
        pool.buffer.reset(ggml_backend_cpu_buffer_from_ptr(pool.memory->data(), pool.memory->size()));
        if (pool.buffer) { ggml_backend_tensor_alloc(pool.buffer.get(), pool.tensor, pool.memory->data()); }
    } else {
        pool.buffer.reset(ggml_backend_alloc_ctx_tensors(pool.context.get(), backend));
    }
    if (!pool.buffer || !pool.tensor->data) {
        throw std::runtime_error("failed to allocate MoE expert slot pool: " + descriptor.name);
    }
    pool.descriptor = &descriptor;
    pool.readiness = std::make_shared<readiness_table>(static_cast<size_t>(n_slots));

    const size_t pool_index = pools.size();
    pool_by_name.emplace(descriptor.name, pool_index);
    pools.push_back(std::move(pool));
    if (overlap) { register_readiness(pools.back().tensor, pools.back().readiness); }
    return pools.back().tensor;
}

int32_t layer_cache::choose_victim(const std::vector<bool> & reserved) const {
    int32_t victim = -1;
    uint64_t oldest = UINT64_MAX;
    for (int32_t slot = 0; slot < n_slots; ++slot) {
        const auto & entry = entries[slot];
        if (reserved[slot] || entry.pinned || entry.active_refs != 0 || entry.state == entry_state::loading) {
            continue;
        }
        if (entry.state == entry_state::empty || entry.state == entry_state::error) {
            return slot;
        }
        if (entry.last_use < oldest) {
            oldest = entry.last_use;
            victim = slot;
        }
    }
    return victim;
}

bool layer_cache::finish_slot(int32_t slot, bool required) {
    bool ok = true;
    for (auto & pool : pools) {
        const auto & read = pool.readiness->reads[slot];
        if (read) { ok = read->finish(required).ok && ok; }
        else { ok = false; }
    }
    if (entries[slot].committed) { entries[slot].state = ok ? entry_state::ready : entry_state::error; }
    return ok;
}

void layer_cache::refresh_completed() {
    for (auto & entry : entries) {
        if (entry.state != entry_state::loading) { continue; }
        bool ready = true;
        for (const auto & pool : pools) {
            const auto & read = pool.readiness->reads[entry.slot_id];
            ready = ready && read && read->ready();
        }
        if (ready && !finish_slot(entry.slot_id, false) && !entry.active_refs) { evict(entry.slot_id); }
    }
}

void layer_cache::queue_slot(int32_t expert, int32_t slot, bool speculative) {
    for (auto & pool : pools) {
        const auto * descriptor = pool.descriptor;
        const uint64_t offset = descriptor->file_offset + static_cast<uint64_t>(expert) * descriptor->expert_stride;
        auto * destination = static_cast<uint8_t *>(pool.tensor->data) + static_cast<size_t>(slot) * pool.tensor->nb[2];
        const size_t length = static_cast<size_t>(descriptor->expert_stride);
        const auto reader = index.reader(descriptor->shard);
        const uint32_t timeout = io_timeout_ms;
        auto ticket = std::make_shared<read_ticket>(executor, counters, [reader, offset, destination, length, timeout] {
            return reader->read_exact(offset, destination, length, timeout);
        }, speculative);
        std::lock_guard<std::mutex> ready_lock(pool.readiness->mutex);
        pool.readiness->reads[slot] = std::move(ticket);
    }
}

void layer_cache::resolve(const int32_t * expert_ids, size_t count, int32_t * slot_ids, bool graph) {
    if (!expert_ids || !slot_ids || !count) { throw std::runtime_error("empty MoE routing request"); }
    std::unique_lock<std::recursive_mutex> lock(cache_mutex(), std::defer_lock);
    if (!lock.try_lock()) { counters->add_wait(); lock.lock(); }
    if (pools.empty()) { throw std::runtime_error("MoE layer has no bound expert tensors"); }
    if (graph && budget) { budget->release_active(); }
    else { release_active(); }
    refresh_completed();
    std::vector<int32_t> unique;
    std::unordered_map<int32_t, int32_t> mapped;
    for (size_t i = 0; i < count; ++i) {
        if (expert_ids[i] < 0 || expert_ids[i] >= n_experts) { throw std::runtime_error("router returned an invalid expert ID"); }
        if (mapped.emplace(expert_ids[i], -1).second) { unique.push_back(expert_ids[i]); }
        else { counters->add_duplicate(); }
    }
    if (unique.size() > static_cast<size_t>(n_slots)) { throw std::runtime_error("MoE slot count is too small for the microbatch"); }
    std::vector<bool> reserved(static_cast<size_t>(n_slots), false);
    std::vector<std::pair<int32_t, int32_t>> misses;
    for (const int32_t expert : unique) {
        const int32_t slot = expert_to_slot[expert];
        if (slot >= 0 && entries[slot].state != entry_state::error) {
            reserved[slot] = true;
            entries[slot].active_refs = 1;
            mapped[expert] = slot;
        }
    }
    try {
        for (const int32_t expert : unique) {
            int32_t slot = mapped[expert];
            if (slot >= 0) {
                counters->add_hit();
                if (entries[slot].prefetched) { counters->add(metric::prefetch_hits); entries[slot].prefetched = false; }
                for (auto & pool : pools) { pool.readiness->reads[slot]->promote(); }
            } else {
                slot = choose_victim(reserved);
                if (slot < 0) {
                    // Cancel unconsumed speculation before rejecting a demand allocation.
                    for (auto & entry : entries) {
                        if (entry.prefetched && !entry.pinned && !entry.active_refs && !reserved[entry.slot_id]) {
                            evict(entry.slot_id); slot = entry.slot_id; break;
                        }
                    }
                }
                if (slot < 0) { throw std::runtime_error("no evictable MoE expert slot is available"); }
                evict(slot);
                commit_slot(slot);
                auto & entry = entries[slot];
                entry.expert_id = expert;
                entry.pinned = pin_requested[expert];
                entry.state = entry_state::loading;
                entry.active_refs = 1;
                ++entry.generation;
                reserved[slot] = true;
                mapped[expert] = slot;
                expert_to_slot[expert] = slot;
                misses.emplace_back(expert, slot);
                counters->add_miss();
            }
            entries[slot].last_use = budget ? ++budget->clock : ++clock;
        }
        std::sort(misses.begin(), misses.end(), [](const auto & a, const auto & b) { return a.second < b.second; });
        for (const auto & miss : misses) { queue_slot(miss.first, miss.second, false); }
        if (graph && overlap) {
            auto batch = std::make_shared<read_batch>();
            for (const int32_t expert : unique) {
                for (const auto & pool : pools) { batch->reads.push_back(pool.readiness->reads[mapped[expert]]); }
            }
            for (const auto & weak : batch->reads) { if (auto read = weak.lock()) { read->set_batch(batch); } }
            counters->add(metric::async_experts, misses.size());
        } else {
            const auto start = std::chrono::steady_clock::now();
            bool ok = true;
            for (const int32_t expert : unique) { ok = finish_slot(mapped[expert], true) && ok; }
            counters->add(metric::remap_wait_us, static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - start).count()));
            if (!ok) { throw std::runtime_error("MoE expert demand read failed"); }
        }
        for (size_t i = 0; i < count; ++i) { slot_ids[i] = mapped.at(expert_ids[i]); }
        previous_experts = unique;
        if (graph && budget) { budget->active = this; }
        else if (!graph) { release_active(); }
    } catch (...) {
        release_active();
        for (const auto & miss : misses) { evict(miss.second); }
        throw;
    }
    lock.unlock();
    if (graph && next_layer_prefetch) { next_layer_prefetch(); }
}

void layer_cache::prefetch_previous(size_t maximum) {
    std::lock_guard<std::recursive_mutex> lock(cache_mutex());
    refresh_completed();
    size_t queued = 0;
    for (const int32_t expert : previous_experts) {
        if (queued >= maximum) { break; }
        if (expert_to_slot[expert] >= 0) { continue; }
        const int32_t slot = choose_victim(std::vector<bool>(static_cast<size_t>(n_slots), false));
        if (slot < 0) { counters->add(metric::prefetch_skipped); break; }
        try {
            evict(slot);
            commit_slot(slot);
            auto & entry = entries[slot];
            entry.expert_id = expert;
            entry.pinned = pin_requested[expert];
            entry.prefetched = true;
            entry.state = entry_state::loading;
            entry.last_use = budget ? ++budget->clock : ++clock;
            expert_to_slot[expert] = slot;
            queue_slot(expert, slot, true);
            ++queued;
        } catch (const std::exception &) {
            evict(slot);
            counters->add(metric::prefetch_skipped);
            break;
        }
    }
}

bool layer_cache::set_pinned(int32_t expert, bool pinned) {
    std::lock_guard<std::recursive_mutex> lock(cache_mutex());
    if (expert < 0 || expert >= n_experts) {
        return false;
    }
    pin_requested[expert] = pinned;
    const int32_t slot = expert_to_slot[expert];
    if (slot >= 0) {
        entries[slot].pinned = pinned;
    }
    return true;
}

int64_t layer_cache::slots() const { return n_slots; }
const stats & layer_cache::get_stats() const { return *counters; }
void layer_cache::reset_stats() { counters->reset(); }

}
