// SPDX-License-Identifier: MIT
// SparkMoE integration extracted from MIT-licensed upstream patches.

#include "moe-integration.h"
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
    if (params.io_mode == LLAMA_MOE_IO_MODE_DIRECT) {
        throw std::runtime_error("direct MoE paging I/O is not implemented; use buffered I/O");
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

void finish_loader(llama_model_loader & loader, llama_model & model) {
    if (!mode_uses_explicit_paging(loader.moe_paging.mode)) {
        return;
    }
    if (loader.paged_index.tensor_count() == 0) {
        throw std::runtime_error("no routed expert tensors were indexed for explicit paging");
    }
    loader.paged_index.open_files(loader.files);
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
    auto manager = std::make_unique<paging_manager>(cpu, params.moe_paging, model.moe_index, model.hparams.n_expert_used);
    const uint32_t safe_ubatch = manager->max_safe_ubatch();
    if (params.n_ubatch > safe_ubatch) {
        LLAMA_LOG_WARN("MoE paging: reducing n_ubatch from %u to %u to fit active experts in the slot cache\n",
                params.n_ubatch, safe_ubatch);
        params.n_ubatch = safe_ubatch;
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
    if (model->moe_index.layer_count() > 0) {
        capabilities->bytes_per_expert_first_layer = model->moe_index.bytes_per_expert(0);
    }
    return true;
}

bool llama_context_moe_paging_get_stats(
        const struct llama_context * ctx,
        struct llama_moe_paging_stats * stats) {
    return ctx && ctx->moe_paging_get_stats(stats);
}

bool llama_context::moe_paging_get_stats(llama_moe_paging_stats * stats) const {
    if (!stats || !moe_paging) {
        return false;
    }
    const auto snapshot = moe_paging->snapshot();
    *stats = {
        snapshot.cache_hits,
        snapshot.cache_misses,
        snapshot.bytes_read,
        snapshot.read_count,
        snapshot.read_latency_us,
        snapshot.evictions,
        snapshot.waits,
        snapshot.duplicate_requests,
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

