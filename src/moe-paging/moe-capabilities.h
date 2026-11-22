// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright (c) 2026 SparkMoE contributors.
#pragma once

namespace llama_moe {
struct architecture_capability {
    const char * architecture;
    const char * evidence;
    bool full_logit_tested;
};

// Evidence describes Spark Edge expert paging rather than general model compatibility.
const architecture_capability & capability_for(const char * architecture);
}
