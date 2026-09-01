// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright (c) 2026 SparkMoE contributors.
#include "moe-capabilities.h"
#include <cstring>

namespace llama_moe {
const architecture_capability & capability_for(const char * architecture) {
    static const architecture_capability registry[] = {
        {"llama", "synthetic F32 MoE full-logit and multi-shard regression", true},
        {"bailingmoe3", "Ling-3.0-tiny Q4_K_M full-logit regression, matching CPU kernels", true},
        {"qwen35moe", "Qwen3.6 IQ4_NL load/generation evidence; full-logit matrix pending", false},
        {"qwen4exp", "bounded PLE and native MTP integration; full-model Q4 validation pending", false},
        {"gemma4", "Gemma 4 Q4_0 load/generation evidence; full-logit matrix pending", false},
    };
    static const architecture_capability fallback{
        "unregistered", "experimental standard routed layout; requires model-specific validation", false};
    for (const auto & entry : registry) {
        if (architecture && std::strcmp(architecture, entry.architecture) == 0) { return entry; }
    }
    return fallback;
}
}
