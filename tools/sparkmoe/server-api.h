// SPDX-License-Identifier: MIT
// SparkMoE integration extracted from MIT-licensed upstream patches.

#pragma once
#include "common.h"
#include "json.h"

const char * sparkmoe_paging_mode_name(llama_moe_paging_mode mode);
common_json sparkmoe_paging_stats_json(const llama_context * context);
common_json sparkmoe_config_json(const common_params & params, const llama_context * context, int slot_n_ctx);
