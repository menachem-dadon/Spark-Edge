// SPDX-License-Identifier: MIT
// SparkMoE integration extracted from MIT-licensed upstream patches.

#include "server-api.h"
#include <algorithm>

using json = common_json;

const char * sparkmoe_paging_mode_name(enum llama_moe_paging_mode mode) {
    switch (mode) {
        case LLAMA_MOE_PAGING_MODE_OFF:      return "off";
        case LLAMA_MOE_PAGING_MODE_EXPLICIT: return "explicit";
        case LLAMA_MOE_PAGING_MODE_OS:       return "os";
        case LLAMA_MOE_PAGING_MODE_AUTO:     return "auto";
    }
    return "unknown";
}

static const char * sparkmoe_io_mode_name(enum llama_moe_io_mode mode) {
    switch (mode) {
        case LLAMA_MOE_IO_MODE_BUFFERED: return "buffered";
        case LLAMA_MOE_IO_MODE_DIRECT:   return "direct";
    }
    return "unknown";
}

json sparkmoe_paging_stats_json(const llama_context * ctx) {
    llama_moe_paging_stats stats{};
    const bool enabled = llama_context_moe_paging_get_stats(ctx, &stats);
    const uint64_t accesses = stats.cache_hits + stats.cache_misses;

    return {
        {"enabled", enabled},
        {"cache_hits", stats.cache_hits},
        {"cache_misses", stats.cache_misses},
        {"cache_hit_rate", accesses == 0 ? 0.0 : (double) stats.cache_hits / (double) accesses},
        {"bytes_read", stats.bytes_read},
        {"read_count", stats.read_count},
        {"read_latency_us", stats.read_latency_us},
        {"average_read_latency_us", stats.read_count == 0 ? 0.0 : (double) stats.read_latency_us / (double) stats.read_count},
        {"evictions", stats.evictions},
        {"waits", stats.waits},
        {"duplicate_requests", stats.duplicate_requests},
    };
}

json sparkmoe_config_json(const common_params & params, const llama_context * context, int slot_n_ctx) {
        const bool mtp_enabled = std::find(
            params.speculative.types.begin(),
            params.speculative.types.end(),
            COMMON_SPECULATIVE_TYPE_DRAFT_MTP) != params.speculative.types.end();
        return {
            {"mode", sparkmoe_paging_mode_name(params.moe_paging.mode)},
            {"io_mode", sparkmoe_io_mode_name(params.moe_paging.io_mode)},
            {"cache_bytes", params.moe_paging.cache_bytes},
            {"slots_per_layer", params.moe_paging.n_slots},
            {"io_threads", params.moe_paging.io_threads},
            {"paged_layers", params.moe_paging.n_layers},
            {"io_timeout_ms", params.moe_paging.io_timeout_ms},
            {"context_size", llama_n_ctx(context)},
            {"context_size_per_slot", slot_n_ctx},
            {"model_context_train", llama_model_n_ctx_train(llama_get_model(context))},
            {"batch_size", llama_n_batch(context)},
            {"effective_ubatch_size", llama_n_ubatch(context)},
            {"parallel", params.n_parallel},
            {"context_shift", params.ctx_shift},
            {"cache_type_k", ggml_type_name(params.cache_type_k)},
            {"cache_type_v", ggml_type_name(params.cache_type_v)},
            {"mtp_enabled", mtp_enabled},
            {"mtp_draft_tokens", params.speculative.draft.n_max},
            {"mtp_min_tokens", params.speculative.draft.n_min},
            {"mtp_p_min", params.speculative.draft.p_min},
        };
}
