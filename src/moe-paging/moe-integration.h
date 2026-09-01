// SPDX-License-Identifier: MIT
// SparkMoE integration extracted from MIT-licensed upstream patches.

#pragma once

#include "moe-paging.h"
#include <string>

struct llama_model_loader;
struct llama_model;
struct llama_cparams;

namespace llama_moe {
bool should_page_tensor(const llama_moe_paging_params & params, const std::string & name);
void prepare_loader(llama_model_loader & loader, const llama_moe_paging_params & params, int64_t experts);
ggml_tensor * create_paged_tensor(llama_model_loader & loader, const ggml_tensor * metadata, int flags);
ggml_tensor * create_ple_tensor(llama_model_loader & loader, const ggml_tensor * metadata);
void finish_loader(llama_model_loader & loader, llama_model & model);
std::unique_ptr<paging_manager> init_context(ggml_backend_t cpu, const llama_model & model, llama_cparams & params);
}
