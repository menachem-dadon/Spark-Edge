// SPDX-License-Identifier: MIT
// SparkMoE integration extracted from MIT-licensed upstream patches.

#include "moe-integration.h"
#include "moe-capabilities.h"
#include "llama-context.h"
#include "llama-cparams.h"
#include "llama-model.h"
#include "llama-model-loader.h"

#include <stdexcept>

namespace llama_moe {

bool should_page_tensor(const llama_moe_paging_params & params, const std::string & name) {
    return mode_uses_explicit_paging(params.mode) && is_pageable_expert_weight(name) &&
        (params.n_layers <= 0 || expert_tensor_layer(name) < params.n_layers);
}

void prepare_loader(llama_model_loader & loader, const llama_moe_paging_params & params, int64_t experts) {
    loader.moe_paging = params;
    if (!mode_uses_explicit_paging(params.mode)) {
        return;
    }
    if (experts <= 0) {
        throw std::runtime_error("explicit MoE paging requires a model with routed experts");
    }
    ggml_init_params metadata{};
    metadata.mem_size = ggml_tensor_overhead() * (loader.weights_map.size() + 32);
    metadata.no_alloc = true;
    loader.ctx_paged.reset(ggml_init(metadata));
    if (!loader.ctx_paged) {
        throw std::runtime_error("failed to allocate paged tensor metadata context");
    }
}

ggml_tensor * create_paged_tensor(llama_model_loader & loader, const ggml_tensor * metadata, int flags) {
    if (!loader.ctx_paged) {
        throw std::runtime_error("paged tensor context was not initialized");
    }
    const bool duplicated = flags & llama_model_loader::TENSOR_DUPLICATED;
    if (duplicated) {
        if (auto * existing = ggml_get_tensor(loader.ctx_paged.get(), metadata->name)) {
            return existing;
        }
    }
    const auto & weight = loader.require_weight(metadata->name);
    loader.paged_index.add_tensor(metadata->name, weight.idx, weight.offs, metadata);
    auto * tensor = ggml_dup_tensor(loader.ctx_paged.get(), metadata);
    ggml_set_name(tensor, metadata->name);
    if (duplicated) {
        loader.size_data += ggml_nbytes(metadata);
    } else {
        loader.n_created++;
    }
    return tensor;
}

ggml_tensor * create_ple_tensor(llama_model_loader & loader, const ggml_tensor * metadata) {
    if (!ggml_is_contiguous(metadata) || metadata->ne[2] != 1 || metadata->ne[3] != 1 || loader.files.empty()) {
        throw std::runtime_error("PLE streaming requires a contiguous GGUF tensor on disk");
    }
    const auto & weight = loader.require_weight(metadata->name);
    auto & file = loader.files.at(weight.idx);
    loader.paging_io = std::make_shared<io_executor>(loader.moe_paging.io_threads > 0 ? loader.moe_paging.io_threads : 1);
    auto reader = make_file_reader(file->file_id(), file->size(), loader.moe_paging.io_mode == LLAMA_MOE_IO_MODE_DIRECT);
    std::vector<row_span> spans{{0, uint64_t(metadata->ne[1]), weight.offs, metadata->nb[1], reader}};
    const uint64_t budget = loader.moe_paging.ple_cache_bytes ? loader.moe_paging.ple_cache_bytes : 64ull * 1024 * 1024;
    const size_t page_bytes = std::max<size_t>(4096, reader->alignment());
    loader.ple_rows = std::make_shared<row_stream>(std::move(spans), metadata->nb[1], budget, 16, loader.paging_io,
            loader.moe_paging.io_timeout_ms ? loader.moe_paging.io_timeout_ms : 30000, page_bytes);
    loader.ctx_rows.reset(ggml_init({ggml_tensor_overhead(), nullptr, true}));
    if (!loader.ctx_rows) { throw std::runtime_error("failed to allocate PLE metadata"); }
    auto * tensor = ggml_dup_tensor(loader.ctx_rows.get(), metadata);
    ggml_set_name(tensor, metadata->name);
    ++loader.n_created;
    LLAMA_LOG_INFO("PLE: streaming %zu bytes from shard %u, offset %zu; cache %llu bytes\n", ggml_nbytes(metadata),
            unsigned(weight.idx), weight.offs, (unsigned long long) loader.ple_rows->snapshot().allocated_bytes);
    return tensor;
}

void finish_loader(llama_model_loader & loader, llama_model & model) {
    model.row_stream_ctx = std::move(loader.ctx_rows);
    model.ple_rows = loader.ple_rows;
    model.paging_io = loader.paging_io;
    if (model.ple_rows) {
        model.tensors_by_name.emplace_back("per_layer_token_embd.weight", model.per_layer_tok_embd);
    }
    if (!mode_uses_explicit_paging(loader.moe_paging.mode)) {
        return;
    }
    if (loader.paged_index.tensor_count() == 0) {
        throw std::runtime_error("no routed expert tensors were indexed for explicit paging");
    }
    loader.paged_index.open_files(loader.files, loader.moe_paging.io_mode == LLAMA_MOE_IO_MODE_DIRECT);
    model.moe_index = std::move(loader.paged_index);
    model.moe_paged_ctx = std::move(loader.ctx_paged);
    for (auto * tensor = ggml_get_first_tensor(model.moe_paged_ctx.get()); tensor;
            tensor = ggml_get_next_tensor(model.moe_paged_ctx.get(), tensor)) {
        model.tensors_by_name.emplace_back(ggml_get_name(tensor), tensor);
    }
}

std::unique_ptr<paging_manager> init_context(ggml_backend_t cpu, const llama_model & model, llama_cparams & params) {
    const bool explicit_mode = mode_uses_explicit_paging(params.moe_paging.mode);
    if (explicit_mode != (model.moe_index.tensor_count() > 0)) {
        throw std::runtime_error("model and context must use the same explicit MoE paging mode");
    }
    if (!explicit_mode) {
        return nullptr;
    }
    const auto & capability = capability_for(model.arch_name().c_str());
    LLAMA_LOG_INFO("MoE paging capability: %s - %s\n", capability.architecture, capability.evidence);
    if ((params.moe_paging.cache_policy == LLAMA_MOE_CACHE_HYBRID || params.moe_paging.overlap ||
            params.moe_paging.temporal_prefetch) && model.n_gpu_layers() > 0) {
        throw std::runtime_error("hybrid/overlap/prefetch currently require CPU-only inference (--n-gpu-layers 0)");
    }
    if (params.n_rs_seq > UINT32_MAX - 2) { throw std::runtime_error("invalid speculative rollback size"); }
    const uint32_t minimum_ubatch = params.n_rs_seq ? params.n_rs_seq + 2 : 1;
    auto manager = std::make_unique<paging_manager>(cpu, params.moe_paging, model.moe_index,
                                                   model.hparams.n_expert_used, minimum_ubatch, model.paging_io);
    const uint32_t safe_ubatch = manager->max_safe_ubatch();
    if (params.n_ubatch > safe_ubatch) {
        LLAMA_LOG_WARN("MoE paging: reducing n_ubatch from %u to %u to fit active experts in the slot cache\n",
                params.n_ubatch, safe_ubatch);
        params.n_ubatch = safe_ubatch;
    }
    if (params.n_ubatch < minimum_ubatch) {
        throw std::runtime_error("MoE paging with recurrent speculation requires effective --ubatch-size >= " +
            std::to_string(minimum_ubatch) + "; increase microbatch and expert cache/slots, or disable speculation");
    }
    return manager;
}

}

bool llama_model_moe_paging_capabilities(
        const struct llama_model * model,
        struct llama_moe_paging_capabilities * capabilities) {
    if (!model || !capabilities) {
        return false;
    }
    *capabilities = {};
    capabilities->supported = model->hparams.n_expert > 0;
    capabilities->explicit_mode_available = capabilities->supported;
    capabilities->os_mode_available = llama_mmap::SUPPORTED;
    capabilities->layer_count = static_cast<int32_t>(model->hparams.n_layer());
    capabilities->expert_count = model->hparams.n_expert;
    capabilities->active_expert_count = model->hparams.n_expert_used;
    capabilities->indexed_tensor_count = model->moe_index.tensor_count();
    for (size_t layer = 0; layer < model->moe_index.layer_count(); ++layer) {
        if (model->moe_index.has_layer(static_cast<int32_t>(layer))) {
            capabilities->bytes_per_expert_first_layer = model->moe_index.bytes_per_expert(static_cast<int32_t>(layer));
            break;
        }
    }
    return true;
}

bool llama_context_moe_paging_get_stats(
        const struct llama_context * ctx,
        struct llama_moe_paging_stats * stats) {
    return ctx && ctx->moe_paging_get_stats(stats);
}

bool llama_context::moe_paging_get_stats(llama_moe_paging_stats * stats) const {
    if (!stats || (!moe_paging && !model.ple_rows)) {
        return false;
    }
    const auto snapshot = moe_paging ? moe_paging->snapshot() : llama_moe::stats_snapshot{};
    const auto ple = model.ple_rows ? model.ple_rows->snapshot() : llama_moe::row_stream_stats{};
    *stats = {
        snapshot.cache_hits,
        snapshot.cache_misses,
        snapshot.bytes_read,
        snapshot.read_count,
        snapshot.read_latency_us,
        snapshot.evictions,
        snapshot.waits,
        snapshot.duplicate_requests,
        snapshot.physical_bytes,
        snapshot.direct_bytes,
        snapshot.fallback_reads,
        snapshot.cache_committed_bytes,
        snapshot.cache_limit_bytes,
        snapshot.resident_experts,
        snapshot.pinned_experts,
        snapshot.resize_pending_bytes,
        snapshot.resize_failures,
        snapshot.io_queue_us,
        snapshot.remap_wait_us,
        snapshot.compute_wait_us,
        snapshot.async_experts,
        snapshot.compute_with_io_pending,
        snapshot.prefetch_reads,
        snapshot.prefetch_hits,
        snapshot.prefetch_wasted,
        snapshot.prefetch_promoted,
        snapshot.prefetch_skipped,
        ple.allocated_bytes,
        ple.cache_hits,
        ple.rows_read,
        ple.bytes_read,
        ple.evictions,
    };
    return true;
}

void llama_context_moe_paging_reset_stats(struct llama_context * ctx) {
    if (ctx) {
        ctx->moe_paging_reset_stats();
    }
}

void llama_context::moe_paging_reset_stats() {
    if (moe_paging) {
        moe_paging->reset_stats();
    }
    if (model.ple_rows) {
        model.ple_rows->reset_stats();
    }
}

bool llama_context_moe_paging_set_expert_pinned(
        struct llama_context * ctx,
        int32_t layer,
        int32_t expert,
        bool pinned) {
    return ctx && ctx->moe_paging_set_expert_pinned(layer, expert, pinned);
}

bool llama_context::moe_paging_set_expert_pinned(int32_t layer, int32_t expert, bool pinned) {
    return moe_paging && moe_paging->set_expert_pinned(layer, expert, pinned);
}

bool llama_context_moe_paging_request_cache_resize(llama_context * ctx, uint64_t bytes) {
    return ctx && ctx->moe_paging_request_cache_resize(bytes);
}

bool llama_context::moe_paging_request_cache_resize(uint64_t bytes) {
    return moe_paging && moe_paging->request_resize(bytes);
}
