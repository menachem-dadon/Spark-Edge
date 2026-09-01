// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright (c) 2026 SparkMoE contributors.

#include "llama.h"
#include "llama-cpp.h"
#include "llama-ext.h"
#include "ggml-cpp.h"
#include "gguf.h"
#include "common.h"
#include "speculative.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

static void require(bool ok, const char * message) {
    if (!ok) { throw std::runtime_error(message); }
}

static void fixture(const char * path, bool moe) {
    ggml_init_params init{2 * 1024 * 1024, nullptr, false};
    ggml_context_ptr ctx(ggml_init(init));
    gguf_context_ptr file(gguf_init_empty());
    gguf_set_val_str(file.get(), "general.architecture", "llama");
    gguf_set_val_str(file.get(), "general.name", "SparkMoE deterministic test fixture");
    gguf_set_val_u32(file.get(), "llama.context_length", 256);
    gguf_set_val_u32(file.get(), "llama.embedding_length", 32);
    gguf_set_val_u32(file.get(), "llama.feed_forward_length", 64);
    gguf_set_val_u32(file.get(), "llama.block_count", 2);
    gguf_set_val_u32(file.get(), "llama.attention.head_count", 4);
    gguf_set_val_u32(file.get(), "llama.attention.head_count_kv", 4);
    gguf_set_val_f32(file.get(), "llama.attention.layer_norm_rms_epsilon", 1e-5f);
    if (moe) {
        gguf_set_val_u32(file.get(), "llama.expert_count", 8);
        gguf_set_val_u32(file.get(), "llama.expert_used_count", 2);
    }
    std::vector<std::string> tokens{"<unk>", "<s>", "</s>"};
    for (int i = 0; i < 256; ++i) {
        char token[16];
        std::snprintf(token, sizeof(token), "<0x%02X>", i);
        tokens.emplace_back(token);
    }
    std::vector<const char *> text;
    for (const auto & token : tokens) { text.push_back(token.c_str()); }
    std::vector<int32_t> types(tokens.size(), LLAMA_TOKEN_TYPE_BYTE);
    types[0] = LLAMA_TOKEN_TYPE_UNKNOWN;
    types[1] = types[2] = LLAMA_TOKEN_TYPE_CONTROL;
    std::vector<float> scores(tokens.size(), 0.0f);
    gguf_set_val_str(file.get(), "tokenizer.ggml.model", "llama");
    gguf_set_arr_str(file.get(), "tokenizer.ggml.tokens", text.data(), text.size());
    gguf_set_arr_data(file.get(), "tokenizer.ggml.scores", GGUF_TYPE_FLOAT32, scores.data(), scores.size());
    gguf_set_arr_data(file.get(), "tokenizer.ggml.token_type", GGUF_TYPE_INT32, types.data(), types.size());
    gguf_set_val_u32(file.get(), "tokenizer.ggml.bos_token_id", 1);
    gguf_set_val_u32(file.get(), "tokenizer.ggml.eos_token_id", 2);
    gguf_set_val_u32(file.get(), "tokenizer.ggml.unknown_token_id", 0);
    uint32_t rng = 42;
    auto tensor = [&](const std::string & name, std::initializer_list<int64_t> dims) {
        std::vector<int64_t> shape(dims);
        auto * t = ggml_new_tensor(ctx.get(), GGML_TYPE_F32, (int) shape.size(), shape.data());
        ggml_set_name(t, name.c_str());
        auto * values = static_cast<float *>(t->data);
        for (int64_t i = 0; i < ggml_nelements(t); ++i) {
            rng = rng * 1664525u + 1013904223u;
            values[i] = name.find("norm") != std::string::npos ? 1.0f :
                (static_cast<int32_t>(rng >> 16) - 32768) / 327680.0f;
            if (name.find(".scale") != std::string::npos) { values[i] = 0.5f + 0.1f * (float) i; }
            // Keep greedy fixture output printable through the HTTP chat parser.
            if (name == "output.weight" && (i / 32 < 35 || i / 32 > 129)) { values[i] = 0; }
        }
        gguf_add_tensor(file.get(), t);
    };
    tensor("token_embd.weight", {32, (int64_t) tokens.size()});
    tensor("output.weight", {32, (int64_t) tokens.size()});
    tensor("output_norm.weight", {32});
    for (int layer = 0; layer < 2; ++layer) {
        const auto prefix = "blk." + std::to_string(layer) + ".";
        tensor(prefix + "attn_norm.weight", {32});
        tensor(prefix + "ffn_norm.weight", {32});
        for (const auto * role : {"attn_q", "attn_k", "attn_v", "attn_output"}) {
            tensor(prefix + role + ".weight", {32, 32});
        }
        if (moe) {
            tensor(prefix + "ffn_gate_inp.weight", {32, 8});
            tensor(prefix + "ffn_gate_exps.weight", {32, 64, 8});
            tensor(prefix + "ffn_up_exps.weight", {32, 64, 8});
            tensor(prefix + "ffn_down_exps.weight", {64, 32, 8});
            for (const auto * role : {"ffn_gate_exps", "ffn_up_exps", "ffn_down_exps"}) {
                tensor(prefix + role + ".scale", {8});
            }
        } else {
            tensor(prefix + "ffn_gate.weight", {32, 64});
            tensor(prefix + "ffn_up.weight", {32, 64});
            tensor(prefix + "ffn_down.weight", {64, 32});
        }
    }
    require(gguf_write_to_file(file.get(), path, false), "cannot write GGUF fixture");
    if (moe) {
        for (int shard = 0; shard < 2; ++shard) {
            gguf_context_ptr split(gguf_init_empty());
            gguf_set_kv(split.get(), file.get());
            gguf_set_val_u16(split.get(), "split.no", static_cast<uint16_t>(shard));
            gguf_set_val_u16(split.get(), "split.count", 2);
            gguf_set_val_i32(split.get(), "split.tensors.count", static_cast<int32_t>(gguf_get_n_tensors(file.get())));
            int ordinal = 0;
            for (auto * t = ggml_get_first_tensor(ctx.get()); t; t = ggml_get_next_tensor(ctx.get(), t), ++ordinal) {
                if (ordinal % 2 == shard) { gguf_add_tensor(split.get(), t); }
            }
            const std::string name = "test-sharded-0000" + std::to_string(shard + 1) + "-of-00002.gguf";
            require(gguf_write_to_file(split.get(), name.c_str(), false), "cannot write GGUF shard");
        }
    }
}

static void qwen_fixture(const char * path, bool draft, ggml_type ple_type = GGML_TYPE_IQ4_NL, bool accepting = false) {
    gguf_context_ptr tokenizer(gguf_init_from_file("test-dense-regression.gguf", {true, nullptr}));
    gguf_context_ptr file(gguf_init_empty());
    gguf_set_kv(file.get(), tokenizer.get());
    gguf_set_val_str(file.get(), "general.architecture", "qwen4exp");
    ggml_context_ptr ctx(ggml_init({16 * 1024 * 1024, nullptr, false}));
    auto key = [&](const char * name, uint32_t value) { gguf_set_val_u32(file.get(), (std::string("qwen4exp.") + name).c_str(), value); };
    key("context_length", 256); key("embedding_length", 128); key("feed_forward_length", 64);
    key("block_count", draft ? 3 : 2); key("attention.head_count", 4); key("attention.head_count_kv", 2);
    key("expert_count", 8); key("expert_used_count", 2);
    key("expert_feed_forward_length", 64); key("expert_shared_feed_forward_length", 64);
    key("ssm.conv_kernel", 4); key("ssm.inner_size", 256); key("ssm.state_size", 128);
    key("ssm.time_step_rank", 2); key("ssm.group_count", 2);
    key("hyper_connection.count", 4); key("hyper_connection.low_rank", 8);
    key("attention.indexer.head_count", 1); key("attention.indexer.key_length", 32); key("attention.indexer.top_k", 4);
    key("full_attention_interval", 2);
    gguf_set_val_f32(file.get(), "qwen4exp.attention.layer_norm_rms_epsilon", 1e-5f);
    const uint32_t sections[] = {8, 8, 8, 8}, ratios[] = {2, 2, 2}, ple_layers[] = {0};
    gguf_set_arr_data(file.get(), "qwen4exp.rope.dimension_sections", GGUF_TYPE_UINT32, sections, 4);
    gguf_set_arr_data(file.get(), "qwen4exp.attention.compress_ratios", GGUF_TYPE_UINT32, ratios, draft ? 3 : 2);
    if (draft) { key("nextn_predict_layers", 1); }
    else {
        gguf_set_arr_data(file.get(), "qwen4exp.ple.layers", GGUF_TYPE_UINT32, ple_layers, 1);
        key("ple.ngram_size", 3); key("ple.heads_per_ngram", 2); key("ple.conv_kernel", 3);
        key("ple.eos_token_id", 2); key("embedding_length_per_layer_input", 32);
        const uint64_t multipliers[] = {300000000001ull, 700000000003ull, 1100000000009ull};
        const uint64_t offsets[] = {0, 61, 122, 183}, sizes[] = {61, 61, 61, 61};
        gguf_set_arr_data(file.get(), "qwen4exp.ple.layer_multipliers", GGUF_TYPE_UINT64, multipliers, 3);
        gguf_set_arr_data(file.get(), "qwen4exp.ple.head_offsets", GGUF_TYPE_UINT64, offsets, 4);
        gguf_set_arr_data(file.get(), "qwen4exp.ple.head_vocab_sizes", GGUF_TYPE_UINT64, sizes, 4);
    }
    uint32_t rng = 42;
    auto tensor = [&](const std::string & name, std::initializer_list<int64_t> dims, ggml_type type = GGML_TYPE_F32) {
        const std::vector<int64_t> shape(dims);
        auto * t = ggml_new_tensor(ctx.get(), type, int(shape.size()), shape.data());
        ggml_set_name(t, name.c_str());
        std::vector<float> values(ggml_nelements(t));
        for (size_t i = 0; i < values.size(); ++i) {
            rng = rng * 1664525u + 1013904223u;
            values[i] = name.find("norm") != std::string::npos ? 1.0f : (int32_t(rng >> 16) - 32768) / 655360.0f;
            if (name.find("ssm_a") != std::string::npos && name.find("weight") == std::string::npos) { values[i] = -0.5f; }
            if (name == "output.weight" && (i / 128 < 35 || i / 128 > 129)) { values[i] = 0; }
            if (accepting) {
                if (name == "token_embd.weight") { values[i] = 0.1f; }
                if (name.find("nextn.eh_proj.weight") != std::string::npos) { values[i] = i % 256 < 128 ? 1.0f : 0.0f; }
                if (name == "output.weight") { values[i] = i / 128 == 68 ? 1.0f : 0.0f; }
            }
        }
        ggml_quantize_chunk(type, values.data(), t->data, 0, values.size() / t->ne[0], t->ne[0], nullptr);
        gguf_add_tensor(file.get(), t);
    };
    tensor("token_embd.weight", {128, 259}); tensor("output.weight", {128, 259});
    tensor("output_hc_norm.weight", {512}); tensor("output_hc_down.weight", {512, 8}); tensor("output_hc_up.weight", {8, 512});
    if (!draft) { tensor("per_layer_token_embd.weight", {32, 256}, ple_type); }
    for (int il = draft ? 2 : 0; il < (draft ? 3 : 2); ++il) {
        const auto p = "blk." + std::to_string(il) + ".";
        for (const auto * part : {"hc_attn", "hc_ffn"}) {
            tensor(p + part + "_norm.weight", {512}); tensor(p + part + "_down.weight", {512, 8});
            tensor(p + part + "_up.weight", {8, 512}); tensor(p + part + "_inject.weight", {512, 4});
        }
        if (il != 0) {
            tensor(p + "attn_q.weight", {128, 256}); tensor(p + "attn_k.weight", {128, 64});
            tensor(p + "attn_v.weight", {128, 64}); tensor(p + "attn_output.weight", {128, 128});
            tensor(p + "attn_q_norm.weight", {32}); tensor(p + "attn_k_norm.weight", {32});
            tensor(p + "indexer.q_proj.weight", {128, 32}); tensor(p + "indexer.k_proj.weight", {128, 32});
            tensor(p + "indexer.q_norm.weight", {32}); tensor(p + "indexer.k_norm.weight", {32});
        } else {
            tensor(p + "attn_qkv.weight", {128, 768}); tensor(p + "attn_gate.weight", {128, 256});
            tensor(p + "ssm_conv1d.weight", {4, 768}); tensor(p + "ssm_dt.bias", {2}); tensor(p + "ssm_a", {2});
            tensor(p + "ssm_beta.weight", {128, 2}); tensor(p + "ssm_alpha.weight", {128, 2});
            tensor(p + "ssm_norm.weight", {128}); tensor(p + "ssm_out.weight", {256, 128});
            tensor(p + "ple_key.weight", {128, 512}); tensor(p + "ple_value.weight", {128, 128});
            for (const auto * role : {"ple_norm_key", "ple_norm_query", "ple_norm_conv"}) { tensor(p + role + ".weight", {512}); }
            tensor(p + "ple_conv1d.weight", {3, 512});
        }
        tensor(p + "ffn_gate_inp.weight", {128, 8});
        tensor(p + "ffn_gate_exps.weight", {128, 64, 8}, GGML_TYPE_Q4_0);
        tensor(p + "ffn_up_exps.weight", {128, 64, 8}, GGML_TYPE_Q4_0);
        tensor(p + "ffn_down_exps.weight", {64, 128, 8}, GGML_TYPE_Q4_0);
        tensor(p + "ffn_gate_inp_shexp.weight", {128});
        tensor(p + "ffn_gate_shexp.weight", {128, 64}); tensor(p + "ffn_up_shexp.weight", {128, 64}); tensor(p + "ffn_down_shexp.weight", {64, 128});
        if (draft) {
            tensor(p + "nextn.eh_proj.weight", {256, 128}); tensor(p + "nextn.enorm.weight", {128}); tensor(p + "nextn.hnorm.weight", {512});
        }
    }
    require(gguf_write_to_file(file.get(), path, false), "cannot write Qwen fixture");
}

static std::vector<float> evaluate(const char * path, llama_moe_paging_mode mode, int layers = 0,
                                  llama_moe_io_mode io_mode = LLAMA_MOE_IO_MODE_BUFFERED, bool synthetic = true,
                                  bool hybrid = false, bool overlap = false, bool prefetch = false, uint32_t real_ubatch = 1,
                                  llama_load_mode load_mode = LLAMA_LOAD_MODE_AUTO) {
    common_params params;
    params.model.path = path;
    params.load_mode = load_mode;
    params.n_ctx = 64;
    params.n_batch = 8;
    params.n_ubatch = 8;
    params.n_parallel = 1;
    params.n_gpu_layers = 0;
    params.cpuparams.n_threads = 2;
    params.cpuparams_batch.n_threads = 2;
    params.moe_paging.mode = mode;
    params.moe_paging.n_slots = 4;
    if (!synthetic) {
        params.n_ubatch = real_ubatch;
        params.moe_paging.n_slots = 0;
        params.moe_paging.cache_bytes = 512ull * 1024 * 1024;
    }
    params.moe_paging.n_layers = layers;
    params.moe_paging.io_mode = io_mode;
    params.moe_paging.io_threads = 4;
    params.moe_paging.cache_policy = hybrid ? LLAMA_MOE_CACHE_HYBRID : LLAMA_MOE_CACHE_PER_LAYER;
    params.moe_paging.overlap = overlap;
    params.moe_paging.temporal_prefetch = prefetch;
    params.warmup = false;
    params.fit_params = false;
    if (!synthetic) { params.no_extra_bufts = true; }
    auto loaded = common_init_from_params(params);
    auto * ctx = loaded->context();
    auto * model = loaded->model();
    require(ctx && model, "fixture model/context load failed");
    llama_moe_paging_capabilities cap{};
    require(llama_model_moe_paging_capabilities(model, &cap), "missing capabilities");
    const bool paging = params.moe_paging.mode == LLAMA_MOE_PAGING_MODE_EXPLICIT;
    if (synthetic) {
        require(cap.indexed_tensor_count == (paging ? (layers == 1 ? 3u : 6u) : 0u), "wrong indexed tensor count");
    } else {
        require((cap.indexed_tensor_count > 0) == paging, "wrong real-model paging activation");
    }
    if (paging && synthetic) {
        require(llama_n_ubatch(ctx) == (hybrid && layers != 1 ? 4u : 2u), "microbatch was not bounded by cache slots");
        auto mismatch = llama_context_default_params();
        mismatch.n_ctx = 64;
        llama_context_ptr invalid(llama_init_from_model(model, mismatch));
        require(!invalid, "context without paging accepted a paged model");
    }
    const int vocab = llama_vocab_n_tokens(llama_model_get_vocab(model));
    std::vector<float> logits;
    std::vector<llama_token> prompt{1, 68, 69, 70};
    require(llama_decode(ctx, llama_batch_get_one(prompt.data(), (int) prompt.size())) == 0, "prompt decode failed");
    for (int step = 0; step < 12; ++step) {
        if (hybrid && step == 3) {
            require(!llama_context_moe_paging_request_cache_resize(ctx, 1), "unsafe resize was accepted");
            require(llama_context_moe_paging_request_cache_resize(ctx, synthetic ? 98304 : 448ull * 1024 * 1024), "safe shrink was rejected");
        }
        if (hybrid && step == 7) {
            require(llama_context_moe_paging_request_cache_resize(ctx, synthetic ? 196608 : 512ull * 1024 * 1024), "safe grow was rejected");
        }
        const auto * row = llama_get_logits_ith(ctx, -1);
        require(row != nullptr, "missing logits");
        for (int j = 0; j < vocab; ++j) {
            require(std::isfinite(row[j]), "non-finite logits");
            logits.push_back(row[j]);
        }
        llama_token next = (llama_token) (std::max_element(row + 3, row + vocab) - row);
        if (!synthetic) { next = 68 + step; } // Identical teacher-forced inputs across modes.
        require(llama_decode(ctx, llama_batch_get_one(&next, 1)) == 0, "generation decode failed");
    }
    llama_moe_paging_stats stats{};
    require(llama_context_moe_paging_get_stats(ctx, &stats) == paging, "paging enabled in the wrong mode");
    if (paging) {
        require(stats.cache_committed_bytes && stats.resident_experts, "cache residency telemetry is empty");
        if (overlap) { require(stats.async_experts > 0, "async reads did not run"); }
        if (hybrid) {
            require(stats.cache_committed_bytes <= stats.cache_limit_bytes && !stats.resize_pending_bytes && !stats.resize_failures,
                "hybrid budget/resize invariant failed");
        }
        require(stats.cache_hits && stats.cache_misses && stats.bytes_read && stats.read_count, "cache or I/O inactive");
        if (synthetic || !hybrid) { require(stats.evictions > 0, "fixture did not exercise cache eviction"); }
        std::printf("paging cache: %llu committed bytes, %llu hits, %llu misses, %llu evictions\n",
            (unsigned long long) stats.cache_committed_bytes, (unsigned long long) stats.cache_hits,
            (unsigned long long) stats.cache_misses, (unsigned long long) stats.evictions);
        bool pin_ok = false;
        for (int layer = 0; layer < cap.layer_count && !pin_ok; ++layer) {
            if (llama_context_moe_paging_set_expert_pinned(ctx, layer, 0, true)) {
                require(llama_context_moe_paging_set_expert_pinned(ctx, layer, 0, false), "unpin failed");
                pin_ok = true;
            }
        }
        require(pin_ok, "pin failed on every routed layer");
        llama_context_moe_paging_reset_stats(ctx);
        require(llama_context_moe_paging_get_stats(ctx, &stats) && stats.bytes_read == 0, "stats reset failed");
    }
    return logits;
}

static void parity(const std::vector<float> & a, const std::vector<float> & b, bool strict = true) {
    require(a.size() == b.size() && !a.empty(), "empty or inconsistent output");
    float max_error = 0;
    size_t errors = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        max_error = std::max(max_error, std::abs(a[i] - b[i]));
        errors += std::abs(a[i] - b[i]) > 1e-4f + 1e-4f * std::abs(a[i]);
    }
    std::printf("logits parity: %zu values, max abs error %.8g\n", a.size(), max_error);
    if (strict) { require(errors == 0, "paged logits differ from resident baseline"); }
}

static std::vector<float> qwen_evaluate(const char * path, bool paging, bool mtp, bool direct = false, std::vector<float> * draft_rows = nullptr) {
    common_params params;
    params.model.path = path;
    params.n_ctx = 256; params.n_batch = 16; params.n_ubatch = 2; params.n_parallel = 1;
    params.n_gpu_layers = 0; params.cpuparams.n_threads = 2; params.cpuparams_batch.n_threads = 2;
    params.fit_params = false; params.warmup = false; params.no_extra_bufts = true;
    params.load_mode = LLAMA_LOAD_MODE_NONE;
    params.moe_paging.mode = paging ? LLAMA_MOE_PAGING_MODE_EXPLICIT : LLAMA_MOE_PAGING_MODE_OFF;
    params.moe_paging.io_mode = direct ? LLAMA_MOE_IO_MODE_DIRECT : LLAMA_MOE_IO_MODE_BUFFERED;
    params.moe_paging.n_slots = 4;
    params.speculative.types = mtp ? std::vector<common_speculative_type>{COMMON_SPECULATIVE_TYPE_DRAFT_MTP} : std::vector<common_speculative_type>{};
    params.speculative.draft.mparams.path = "test-qwen38-mtp.gguf";
    auto loaded = common_init_from_params(params);
    auto * ctx = loaded->context();
    require(ctx, "Qwen context load failed");
    llama_set_embeddings_nextn(ctx, true, false);
    std::vector<llama_token> prompt{1, 68, 2, 69};
    require(llama_decode(ctx, llama_batch_get_one(prompt.data(), int(prompt.size()))) == 0, "Qwen prefill failed");
    const auto * hidden = llama_get_embeddings_nextn(ctx);
    require(hidden, "missing Qwen wide hidden rows");
    std::vector<float> saved_hidden(hidden, hidden + prompt.size() * 512);
    for (const auto value : saved_hidden) { require(std::isfinite(value), "invalid hidden row"); }
    std::vector<float> result;
    for (llama_token token : {70, 71, 2, 72, 73}) {
        const auto flags = LLAMA_STATE_SEQ_FLAGS_PARTIAL_ONLY;
        std::vector<uint8_t> state(llama_state_seq_get_size_ext(ctx, 0, flags));
        require(llama_state_seq_get_data_ext(ctx, state.data(), state.size(), 0, flags) == state.size(), "checkpoint save failed");
        require(llama_decode(ctx, llama_batch_get_one(&token, 1)) == 0, "Qwen continuation failed");
        const auto * row = llama_get_logits_ith(ctx, -1);
        std::vector<float> expected(row, row + 259);
        require(llama_state_seq_set_data_ext(ctx, state.data(), state.size(), 0, flags) == state.size(), "checkpoint restore failed");
        require(llama_decode(ctx, llama_batch_get_one(&token, 1)) == 0, "Qwen replay failed");
        row = llama_get_logits_ith(ctx, -1);
        parity(expected, std::vector<float>(row, row + 259));
        result.insert(result.end(), row, row + 259);
    }
    llama_moe_paging_stats ple_stats{};
    require(llama_context_moe_paging_get_stats(ctx, &ple_stats), "PLE telemetry is unavailable");
    require(ple_stats.ple_cache_bytes && ple_stats.ple_pages_read && ple_stats.ple_bytes_read, "PLE telemetry is inactive");
    if (mtp) {
        auto params_dft = common_base_params_to_speculative(params);
        params_dft.n_outputs_max = 4;
        params_dft.n_outputs_max_per_seq = 4;
        common_speculative_init_result draft(params_dft, loaded->model(), ctx);
        require(draft.context(), "Qwen MTP sidecar load failed");
        llama_set_embeddings_nextn(draft.context(), true, true);
        auto batch = llama_batch_init(4, 512, 1);
        batch.token = static_cast<llama_token *>(std::malloc(4 * sizeof(llama_token)));
        require(batch.token, "MTP token allocation failed");
        batch.n_tokens = 4;
        std::copy(saved_hidden.begin(), saved_hidden.end(), batch.embd);
        for (int i = 0; i < 4; ++i) {
            batch.token[i] = 70 + i; batch.pos[i] = i; batch.n_seq_id[i] = 1; batch.seq_id[i][0] = 0; batch.logits[i] = 1;
        }
        const int code = llama_decode(draft.context(), batch);
        llama_batch_free(batch);
        require(code == 0, "Qwen MTP forward failed");
        const auto * logits = llama_get_logits(draft.context());
        require(logits && llama_get_embeddings_nextn(draft.context()), "MTP outputs missing");
        for (int i = 0; i < 4 * 259; ++i) { require(std::isfinite(logits[i]), "invalid MTP logits"); }
        if (draft_rows) { draft_rows->assign(logits, logits + 4 * 259); }
    }
    return result;
}

int main(int argc, char ** argv) {
    try {
        llama_backend_init();
        if (argc == 3 && std::string(argv[1]) == "--residency") {
            const auto mapped = evaluate(argv[2], LLAMA_MOE_PAGING_MODE_EXPLICIT, 0, LLAMA_MOE_IO_MODE_BUFFERED,
                                         false, false, false, false, 1, LLAMA_LOAD_MODE_MMAP);
            parity(mapped, evaluate(argv[2], LLAMA_MOE_PAGING_MODE_EXPLICIT, 0, LLAMA_MOE_IO_MODE_BUFFERED,
                                    false, false, false, false, 1, LLAMA_LOAD_MODE_NONE));
            llama_backend_free();
            std::puts("SparkMoE dense/shared residency full-logit regression PASS");
            return 0;
        }
        if (argc == 3 && std::string(argv[1]) == "--compare-batching") {
            const auto single = evaluate(argv[2], LLAMA_MOE_PAGING_MODE_OFF, 0, LLAMA_MOE_IO_MODE_BUFFERED, false);
            const auto batched = evaluate(argv[2], LLAMA_MOE_PAGING_MODE_OFF, 0, LLAMA_MOE_IO_MODE_BUFFERED, false, false, false, false, 8);
            std::puts("Resident-only microbatch 1 versus 8 (diagnostic, not a paging gate):");
            parity(single, batched, false);
            parity(single, evaluate(argv[2], LLAMA_MOE_PAGING_MODE_EXPLICIT, 0, LLAMA_MOE_IO_MODE_BUFFERED, false));
            llama_backend_free();
            return 0;
        }
        if (argc == 3 && std::string(argv[1]) == "--model") {
            const auto baseline = evaluate(argv[2], LLAMA_MOE_PAGING_MODE_OFF, 0, LLAMA_MOE_IO_MODE_BUFFERED, false);
            parity(baseline, evaluate(argv[2], LLAMA_MOE_PAGING_MODE_EXPLICIT, 0, LLAMA_MOE_IO_MODE_BUFFERED, false));
            parity(baseline, evaluate(argv[2], LLAMA_MOE_PAGING_MODE_EXPLICIT, 0, LLAMA_MOE_IO_MODE_DIRECT, false));
            parity(baseline, evaluate(argv[2], LLAMA_MOE_PAGING_MODE_EXPLICIT, 0, LLAMA_MOE_IO_MODE_BUFFERED, false, true));
            parity(baseline, evaluate(argv[2], LLAMA_MOE_PAGING_MODE_EXPLICIT, 0, LLAMA_MOE_IO_MODE_BUFFERED, false, false, true));
            parity(baseline, evaluate(argv[2], LLAMA_MOE_PAGING_MODE_EXPLICIT, 0, LLAMA_MOE_IO_MODE_BUFFERED, false, true, true, true));
            llama_backend_free();
            std::puts("SparkMoE real-model full-logit regression PASS");
            return 0;
        }
        fixture("test-moe-regression.gguf", true);
        fixture("test-dense-regression.gguf", false);
        if (argc == 2 && std::string(argv[1]) == "--qwen") {
            qwen_fixture("test-qwen38.gguf", false);
            qwen_fixture("test-qwen38-mtp.gguf", true);
            qwen_fixture("test-qwen38-q5.gguf", false, GGML_TYPE_Q5_0);
            const auto baseline = qwen_evaluate("test-qwen38.gguf", false, false);
            std::vector<float> draft_resident, draft_paged;
            parity(baseline, qwen_evaluate("test-qwen38.gguf", false, true, false, &draft_resident));
            parity(baseline, qwen_evaluate("test-qwen38.gguf", true, false));
            parity(baseline, qwen_evaluate("test-qwen38.gguf", true, true, true, &draft_paged));
            parity(draft_resident, draft_paged);
            parity(qwen_evaluate("test-qwen38-q5.gguf", false, false), qwen_evaluate("test-qwen38-q5.gguf", true, true, true));
            std::puts("SparkMoE Qwen PLE, paging, checkpoint and MTP regression PASS");
            return 0;
        }
        if (argc == 2 && std::string(argv[1]) == "--qwen-fixtures-only") {
            qwen_fixture("test-qwen38.gguf", false);
            qwen_fixture("test-qwen38-mtp.gguf", true);
            qwen_fixture("test-qwen38-q5.gguf", false, GGML_TYPE_Q5_0);
            qwen_fixture("test-qwen38-mtp-accept.gguf", true, GGML_TYPE_IQ4_NL, true);
            return 0;
        }
        if (argc == 2 && std::string(argv[1]) == "--fixtures-only") {
            llama_backend_free();
            return 0;
        }
        const auto resident = evaluate("test-moe-regression.gguf", LLAMA_MOE_PAGING_MODE_OFF);
        parity(resident, evaluate("test-moe-regression.gguf", LLAMA_MOE_PAGING_MODE_EXPLICIT));
        parity(resident, evaluate("test-moe-regression.gguf", LLAMA_MOE_PAGING_MODE_EXPLICIT, 1));
        parity(resident, evaluate("test-moe-regression.gguf", LLAMA_MOE_PAGING_MODE_EXPLICIT, 0, LLAMA_MOE_IO_MODE_DIRECT));
        parity(resident, evaluate("test-sharded-00001-of-00002.gguf", LLAMA_MOE_PAGING_MODE_EXPLICIT, 0, LLAMA_MOE_IO_MODE_DIRECT));
        parity(resident, evaluate("test-sharded-00001-of-00002.gguf", LLAMA_MOE_PAGING_MODE_EXPLICIT, 0, LLAMA_MOE_IO_MODE_DIRECT, true, true));
        parity(resident, evaluate("test-moe-regression.gguf", LLAMA_MOE_PAGING_MODE_EXPLICIT, 0, LLAMA_MOE_IO_MODE_BUFFERED, true, false, true));
        parity(resident, evaluate("test-moe-regression.gguf", LLAMA_MOE_PAGING_MODE_EXPLICIT, 0, LLAMA_MOE_IO_MODE_BUFFERED, true, false, false, true));
        parity(resident, evaluate("test-sharded-00001-of-00002.gguf", LLAMA_MOE_PAGING_MODE_EXPLICIT, 0, LLAMA_MOE_IO_MODE_DIRECT, true, true, true, true));
        parity(resident, evaluate("test-moe-regression.gguf", LLAMA_MOE_PAGING_MODE_OS));
        parity(resident, evaluate("test-moe-regression.gguf", LLAMA_MOE_PAGING_MODE_AUTO));
        parity(evaluate("test-dense-regression.gguf", LLAMA_MOE_PAGING_MODE_OFF),
               evaluate("test-dense-regression.gguf", LLAMA_MOE_PAGING_MODE_AUTO));
        std::remove("test-moe-regression.gguf");
        std::remove("test-dense-regression.gguf");
        std::remove("test-sharded-00001-of-00002.gguf");
        std::remove("test-sharded-00002-of-00002.gguf");
        llama_backend_free();
        std::puts("SparkMoE model/graph regression PASS");
        return 0;
    } catch (const std::exception & error) {
        std::fprintf(stderr, "SparkMoE regression: %s\n", error.what());
        return 1;
    }
}
