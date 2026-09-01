// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright (c) 2026 SparkMoE contributors.
#pragma once
#include "moe-paging/moe-row-stream.h"
#include "ggml-cpu.h"
#include "ggml-cpp.h"
#include <atomic>
#include <cstring>
#include <stdexcept>
#include <vector>

class sparse_row_reader : public llama_moe::file_reader {
public:
    uint64_t size() const override { return 512ull * 1024 * 1024 * 1024; }
    std::atomic<uint64_t> calls{0}, largest{0};
    std::atomic<bool> fail{false};
    llama_moe::io_result read_exact(uint64_t offset, void * output, size_t length, uint32_t) override {
        ++calls;
        uint64_t old = largest.load();
        while (old < length && !largest.compare_exchange_weak(old, length)) {}
        llama_moe::io_result result;
        if (fail.exchange(false)) { result.error = "injected storage failure"; return result; }
        if (offset > size() || length > size() - offset) { return result; }
        for (size_t i = 0; i < length; ++i) { static_cast<uint8_t *>(output)[i] = static_cast<uint8_t>((offset >> 4) + i); }
        result.ok = true; result.bytes = length;
        return result;
    }
};

class quantized_row_reader : public llama_moe::file_reader {
public:
    std::vector<uint8_t> data;
    uint64_t size() const override { return data.size(); }
    llama_moe::io_result read_exact(uint64_t offset, void * output, size_t length, uint32_t) override {
        llama_moe::io_result result;
        if (offset > data.size() || length > data.size() - offset) { return result; }
        std::memcpy(output, data.data() + offset, length);
        result.ok = true; result.bytes = length;
        return result;
    }
};

inline void check_quantized_rows() {
    auto executor = std::make_shared<llama_moe::io_executor>(2);
    for (auto type : {GGML_TYPE_IQ4_NL, GGML_TYPE_Q5_0, GGML_TYPE_F32}) {
        ggml_context_ptr ctx(ggml_init({2 * 1024 * 1024, nullptr, false}));
        auto * weights = ggml_new_tensor_2d(ctx.get(), type, 160, 64);
        std::vector<float> values(160 * 64);
        for (size_t i = 0; i < values.size(); ++i) { values[i] = float(int(i * 17 % 97) - 48) / 64; }
        ggml_quantize_chunk(type, values.data(), weights->data, 0, 64, 160, nullptr);
        auto reader = std::make_shared<quantized_row_reader>();
        reader->data.resize(4095 + ggml_nbytes(weights));
        std::memcpy(reader->data.data() + 4095, weights->data, ggml_nbytes(weights));
        llama_moe::row_stream stream({{0, 64, 4095, weights->nb[1], reader}}, weights->nb[1], 12288, 2, executor, 30000, 4096);
        auto * indices = ggml_new_tensor_1d(ctx.get(), GGML_TYPE_I32, 257);
        auto * ids = static_cast<int32_t *>(indices->data);
        for (int i = 0; i < 257; ++i) { ids[i] = i * 17 % 64; }
        auto * expected = ggml_get_rows(ctx.get(), weights, indices);
        auto * graph = ggml_new_graph(ctx.get());
        ggml_build_forward_expand(graph, expected);
        if (ggml_graph_compute_with_ctx(ctx.get(), graph, 2) != GGML_STATUS_SUCCESS) { throw std::runtime_error("GET_ROWS reference failed"); }
        std::vector<float> actual(160 * 257);
        stream.copy_rows_f32(ids, 257, type, 160, actual.data());
        if (std::memcmp(actual.data(), expected->data, actual.size() * sizeof(float)) || stream.snapshot().allocated_bytes > 12288) {
            throw std::runtime_error("quantized page gather differs from GGML GET_ROWS");
        }
    }
}

inline void check_bounded_rows() {
    check_quantized_rows();
    auto require = [](bool value) { if (!value) { throw std::runtime_error("bounded row regression failed"); } };
    auto executor = std::make_shared<llama_moe::io_executor>(4);
    auto a = std::make_shared<sparse_row_reader>();
    auto b = std::make_shared<sparse_row_reader>();
    const uint64_t half = 1ull << 31;
    std::vector<llama_moe::row_span> sources{{0, half, 128, 80, a}, {half, half, 32, 96, b}};
    llama_moe::row_stream stream(sources, 64, 384, 4, executor);
    require(stream.row_count() == 1ull << 32);
    const uint64_t ids[] = {0, half + 7, half - 1, half + 7, 4, half + 99, 0};
    uint8_t output[sizeof(ids) / sizeof(ids[0]) * 64]{};
    stream.copy_rows(ids, 7, output, sizeof(output));
    for (size_t i = 0; i < 7; ++i) {
        const uint64_t offset = ids[i] < half ? 128 + ids[i] * 80 : 32 + (ids[i] - half) * 96;
        for (size_t j = 0; j < 64; ++j) { require(output[i * 64 + j] == static_cast<uint8_t>((offset >> 4) + j)); }
    }
    const auto before = a->calls.load() + b->calls.load();
    stream.copy_rows(ids, 1, output, sizeof(output));
    require(a->calls + b->calls == before);
    require(stream.snapshot().allocated_bytes <= 384 && stream.snapshot().peak_inflight_rows <= 4);
    require(stream.snapshot().evictions > 0 && stream.snapshot().cache_hits > 0);
    require(a->largest <= 64 && b->largest <= 64 && stream.snapshot().bytes_read < 1024);
    bool failed = false;
    const uint64_t invalid = UINT64_MAX;
    try { stream.copy_rows(&invalid, 1, output, sizeof(output)); } catch (const std::out_of_range &) { failed = true; }
    require(failed && a->calls + b->calls == before);
    failed = false;
    try { llama_moe::row_stream too_small(sources, 64, 1, 4, executor); } catch (const std::invalid_argument &) { failed = true; }
    require(failed);
    auto gap = sources;
    ++gap[1].first_row;
    failed = false;
    try { llama_moe::row_stream missing(gap, 64, 384, 4, executor); } catch (const std::invalid_argument &) { failed = true; }
    require(failed);
    const uint64_t fresh = 333;
    a->fail = true;
    failed = false;
    try { stream.copy_rows(&fresh, 1, output, sizeof(output)); } catch (const std::runtime_error &) { failed = true; }
    require(failed);
    stream.copy_rows(&fresh, 1, output, sizeof(output));
    require(output[0] == static_cast<uint8_t>((128 + fresh * 80) >> 4));
}
