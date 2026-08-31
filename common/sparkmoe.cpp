// SPDX-License-Identifier: MIT
// SparkMoE integration extracted from MIT-licensed upstream patches.

#include "sparkmoe.h"
#include "log.h"
#include <stdexcept>

std::vector<common_arg> sparkmoe_options() {
    std::vector<common_arg> options;
    auto add_opt = [&](common_arg option) { options.push_back(std::move(option)); };
    add_opt(common_arg(
        {"--moe-paging"}, "off|explicit|os|auto",
        "MoE expert paging mode (default: off)",
        [](common_params & params, const std::string & value) {
            if (value == "off") {
                params.moe_paging.mode = LLAMA_MOE_PAGING_MODE_OFF;
            } else if (value == "explicit") {
                params.moe_paging.mode = LLAMA_MOE_PAGING_MODE_EXPLICIT;
            } else if (value == "os") {
                params.moe_paging.mode = LLAMA_MOE_PAGING_MODE_OS;
            } else if (value == "auto") {
                params.moe_paging.mode = LLAMA_MOE_PAGING_MODE_AUTO;
            } else {
                throw std::invalid_argument("invalid MoE paging mode");
            }
        }
    ).set_env("LLAMA_ARG_MOE_PAGING"));
    add_opt(common_arg(
        {"--moe-cache-mib"}, "N",
        "total explicit MoE expert cache budget in MiB",
        [](common_params & params, const std::string & value) {
            const uint64_t mib = std::stoull(value);
            if (mib == 0 || mib > UINT64_MAX / (1024ull * 1024ull)) {
                throw std::invalid_argument("invalid MoE cache size");
            }
            params.moe_paging.cache_bytes = mib * 1024ull * 1024ull;
        }
    ).set_env("LLAMA_ARG_MOE_CACHE_MIB"));
    add_opt(common_arg(
        {"--moe-slots"}, "N",
        "expert slots per MoE layer; must cover all active experts",
        [](common_params & params, int value) {
            if (value <= 0) {
                throw std::invalid_argument("MoE slot count must be positive");
            }
            params.moe_paging.n_slots = value;
        }
    ).set_env("LLAMA_ARG_MOE_SLOTS"));
    add_opt(common_arg(
        {"--moe-io-threads"}, "N",
        "worker threads used for expert reads",
        [](common_params & params, int value) {
            if (value <= 0 || value > 128) {
                throw std::invalid_argument("MoE I/O thread count must be between 1 and 128");
            }
            params.moe_paging.io_threads = value;
        }
    ).set_env("LLAMA_ARG_MOE_IO_THREADS"));
    add_opt(common_arg(
        {"--moe-io-mode"}, "buffered|direct",
        "expert read mode; aligned direct I/O validates bytes and falls back to buffered reads when needed",
        [](common_params & params, const std::string & value) {
            if (value == "buffered") {
                params.moe_paging.io_mode = LLAMA_MOE_IO_MODE_BUFFERED;
            } else if (value == "direct") {
                params.moe_paging.io_mode = LLAMA_MOE_IO_MODE_DIRECT;
            } else {
                throw std::invalid_argument("invalid MoE I/O mode");
            }
        }
    ).set_env("LLAMA_ARG_MOE_IO_MODE"));
    add_opt(common_arg(
        {"--moe-cache-policy"}, "lru",
        "expert cache policy; only exact per-layer LRU is supported",
        [](common_params &, const std::string & value) {
            if (value != "lru") {
                throw std::invalid_argument("only the lru MoE cache policy is supported");
            }
        }
    ).set_env("LLAMA_ARG_MOE_CACHE_POLICY"));
    return options;
}

void sparkmoe_normalize_params(common_params & params) {
    if (params.moe_paging.mode == LLAMA_MOE_PAGING_MODE_AUTO) {
        params.moe_paging.mode = params.moe_paging.n_slots > 0 || params.moe_paging.cache_bytes > 0 ?
            LLAMA_MOE_PAGING_MODE_EXPLICIT : LLAMA_MOE_PAGING_MODE_OS;
        LOG_INF("MoE paging auto selected %s mode because %s\n",
            params.moe_paging.mode == LLAMA_MOE_PAGING_MODE_EXPLICIT ? "explicit" : "os",
            params.moe_paging.mode == LLAMA_MOE_PAGING_MODE_EXPLICIT ? "a cache limit was provided" : "no explicit cache limit was provided");
    }
    if (params.moe_paging.mode == LLAMA_MOE_PAGING_MODE_EXPLICIT &&
            params.moe_paging.n_slots <= 0 && params.moe_paging.cache_bytes == 0) {
        params.moe_paging.n_slots = 8;
        LOG_INF("MoE paging: using 8 slots per layer; the active expert count can raise this minimum\n");
    }
    if (params.moe_paging.mode == LLAMA_MOE_PAGING_MODE_OS) {
        params.load_mode = LLAMA_LOAD_MODE_MMAP;
    }

}
