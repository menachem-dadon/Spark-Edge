// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright (c) 2026 SparkMoE contributors.

#include "llama.h"
#include "llama-cpp.h"
#include "ggml-cpp.h"
#include "gguf.h"
#include "common.h"

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

static std::vector<float> evaluate(const char * path, llama_moe_paging_mode mode, int layers = 0,
                                  llama_moe_io_mode io_mode = LLAMA_MOE_IO_MODE_BUFFERED, bool synthetic = true) {
    common_params params;
    params.model.path = path;
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
        params.moe_paging.n_slots = 0;
        params.moe_paging.cache_bytes = 1500ull * 1024 * 1024;
    }
    params.moe_paging.n_layers = layers;
    params.moe_paging.io_mode = io_mode;
    params.moe_paging.io_threads = 4;
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
        require(llama_n_ubatch(ctx) == 2, "microbatch was not bounded by cache slots");
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
        require(stats.cache_hits && stats.cache_misses && stats.bytes_read && stats.read_count, "cache or I/O inactive");
        require(stats.evictions > 0, "fixture did not exercise cache eviction");
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

static void parity(const std::vector<float> & a, const std::vector<float> & b) {
    require(a.size() == b.size() && !a.empty(), "empty or inconsistent output");
    float max_error = 0;
    size_t errors = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        max_error = std::max(max_error, std::abs(a[i] - b[i]));
        errors += std::abs(a[i] - b[i]) > 1e-4f + 1e-4f * std::abs(a[i]);
    }
    std::printf("logits parity: %zu values, max abs error %.8g\n", a.size(), max_error);
    require(errors == 0, "paged logits differ from resident baseline");
}

int main(int argc, char ** argv) {
    try {
        llama_backend_init();
        if (argc == 3 && std::string(argv[1]) == "--model") {
            const auto baseline = evaluate(argv[2], LLAMA_MOE_PAGING_MODE_OFF, 0, LLAMA_MOE_IO_MODE_BUFFERED, false);
            parity(baseline, evaluate(argv[2], LLAMA_MOE_PAGING_MODE_EXPLICIT, 0, LLAMA_MOE_IO_MODE_BUFFERED, false));
            parity(baseline, evaluate(argv[2], LLAMA_MOE_PAGING_MODE_EXPLICIT, 0, LLAMA_MOE_IO_MODE_DIRECT, false));
            llama_backend_free();
            std::puts("SparkMoE real-model full-logit regression PASS");
            return 0;
        }
        fixture("test-moe-regression.gguf", true);
        fixture("test-dense-regression.gguf", false);
        if (argc == 2 && std::string(argv[1]) == "--fixtures-only") {
            llama_backend_free();
            return 0;
        }
        const auto resident = evaluate("test-moe-regression.gguf", LLAMA_MOE_PAGING_MODE_OFF);
        parity(resident, evaluate("test-moe-regression.gguf", LLAMA_MOE_PAGING_MODE_EXPLICIT));
        parity(resident, evaluate("test-moe-regression.gguf", LLAMA_MOE_PAGING_MODE_EXPLICIT, 1));
        parity(resident, evaluate("test-moe-regression.gguf", LLAMA_MOE_PAGING_MODE_EXPLICIT, 0, LLAMA_MOE_IO_MODE_DIRECT));
        parity(resident, evaluate("test-sharded-00001-of-00002.gguf", LLAMA_MOE_PAGING_MODE_EXPLICIT, 0, LLAMA_MOE_IO_MODE_DIRECT));
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
