#include "ggml-backend.h"
#include "ggml-cpp.h"
#include "llama-mmap.h"
#include "llama.h"
#include "moe-paging/moe-cache.h"
#include "moe-paging/moe-model-index.h"
#include "moe-paging/moe-paging.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

#define REQUIRE(expression) do { \
    if (!(expression)) { \
        std::fprintf(stderr, "requirement failed at line %d: %s\n", __LINE__, #expression); \
        return 1; \
    } \
} while (0)

int main() {
    const char * path = "test-moe-paging-fixture.bin";
    const char * shard_path = "test-moe-paging-fixture-shard-1.bin";

    ggml_init_params metadata_params{};
    metadata_params.mem_size = ggml_tensor_overhead() * 4;
    metadata_params.no_alloc = true;
    ggml_context_ptr metadata(ggml_init(metadata_params));
    REQUIRE(metadata);

    auto * original = ggml_new_tensor_3d(metadata.get(), GGML_TYPE_F32, 2, 2, 4);
    ggml_set_name(original, "blk.0.ffn_up_exps.weight");
    REQUIRE(original->nb[2] == 4 * sizeof(float));

    auto * sharded = ggml_new_tensor_3d(metadata.get(), GGML_TYPE_F32, 2, 2, 4);
    ggml_set_name(sharded, "blk.1.ffn_down_exps.weight");
    REQUIRE(sharded->nb[2] == original->nb[2]);

    std::vector<uint8_t> file_bytes(64 + ggml_nbytes(original), 0);
    for (int expert = 0; expert < 4; ++expert) {
        float values[4];
        for (int i = 0; i < 4; ++i) {
            values[i] = static_cast<float>(expert * 10 + i);
        }
        std::memcpy(file_bytes.data() + 64 + expert * original->nb[2], values, sizeof(values));
    }
    {
        llama_file output(path, "wb");
        output.write_raw(file_bytes.data(), file_bytes.size());
    }

    std::vector<uint8_t> shard_bytes(32 + ggml_nbytes(sharded), 0);
    for (int expert = 0; expert < 4; ++expert) {
        float values[4];
        for (int i = 0; i < 4; ++i) {
            values[i] = static_cast<float>(100 + expert * 10 + i);
        }
        std::memcpy(shard_bytes.data() + 32 + expert * sharded->nb[2], values, sizeof(values));
    }
    {
        llama_file output(shard_path, "wb");
        output.write_raw(shard_bytes.data(), shard_bytes.size());
    }

    llama_moe::model_index index;
    index.add_tensor(original->name, 0, 64, original);
    index.add_tensor(sharded->name, 1, 32, sharded);
    std::vector<std::unique_ptr<llama_file>> files;
    files.push_back(std::make_unique<llama_file>(path, "rb"));
    files.push_back(std::make_unique<llama_file>(shard_path, "rb"));
    index.open_files(files);
    REQUIRE(index.tensor_count() == 2);
    REQUIRE(index.layer_count() == 2);

    llama_backend_init();
    ggml_backend_ptr backend(ggml_backend_init_by_type(GGML_BACKEND_DEVICE_TYPE_CPU, nullptr));
    REQUIRE(backend);

    {
    llama_moe::layer_cache cache(0, 4, 2, 2, 30000, index, backend.get());
    auto * pool = cache.bind_tensor(original, *index.find(original->name));
    REQUIRE(pool && pool->data);

    const int32_t first_ids[] = {2, 3};
    int32_t first_slots[2] = {-1, -1};
    cache.resolve(first_ids, 2, first_slots);
    REQUIRE(first_slots[0] != first_slots[1]);
    for (int request = 0; request < 2; ++request) {
        const auto * values = reinterpret_cast<const float *>(
            static_cast<const uint8_t *>(pool->data) + first_slots[request] * pool->nb[2]);
        const auto * resident_values = reinterpret_cast<const float *>(
            file_bytes.data() + 64 + first_ids[request] * original->nb[2]);
        const float input[] = {0.5f, -1.0f, 2.0f, 0.25f};
        float resident_output = 0.0f;
        float paged_output = 0.0f;
        for (int i = 0; i < 4; ++i) {
            REQUIRE(values[i] == static_cast<float>(first_ids[request] * 10 + i));
            resident_output += resident_values[i] * input[i];
            paged_output += values[i] * input[i];
        }
        REQUIRE(paged_output == resident_output);
    }

    int32_t hit_slot = -1;
    cache.resolve(first_ids, 1, &hit_slot);
    REQUIRE(hit_slot == first_slots[0]);

    const int32_t second_ids[] = {0, 1};
    int32_t second_slots[2] = {-1, -1};
    cache.resolve(second_ids, 2, second_slots);
    REQUIRE(second_slots[0] != second_slots[1]);

    bool rejected = false;
    try {
        const int32_t oversized_ids[] = {0, 1, 2};
        int32_t oversized_slots[3];
        cache.resolve(oversized_ids, 3, oversized_slots);
    } catch (const std::runtime_error &) {
        rejected = true;
    }
    REQUIRE(rejected);

    const auto stats = cache.get_stats().snapshot();
    REQUIRE(stats.cache_hits >= 1);
    REQUIRE(stats.cache_misses == 4);
    REQUIRE(stats.read_count == 4);
    REQUIRE(stats.bytes_read == 4 * original->nb[2]);
    REQUIRE(stats.evictions == 2);

    const int32_t duplicate_ids[] = {0, 0};
    int32_t duplicate_slots[2] = {-1, -1};
    cache.resolve(duplicate_ids, 2, duplicate_slots);
    REQUIRE(duplicate_slots[0] == duplicate_slots[1]);
    REQUIRE(cache.get_stats().snapshot().duplicate_requests == 1);

    REQUIRE(cache.set_pinned(0, true));
    const int32_t new_id = 2;
    int32_t new_slot = -1;
    cache.resolve(&new_id, 1, &new_slot);
    int32_t pinned_slot = -1;
    const int32_t pinned_id = 0;
    cache.resolve(&pinned_id, 1, &pinned_slot);
    REQUIRE(pinned_slot == duplicate_slots[0]);
    REQUIRE(cache.set_pinned(0, false));
    REQUIRE(!cache.set_pinned(99, true));

    bool invalid_id_rejected = false;
    try {
        const int32_t invalid_id = -1;
        int32_t invalid_slot = -1;
        cache.resolve(&invalid_id, 1, &invalid_slot);
    } catch (const std::runtime_error &) {
        invalid_id_rejected = true;
    }
    REQUIRE(invalid_id_rejected);
    }

    {
    llama_moe::layer_cache shard_cache(1, 4, 2, 2, 30000, index, backend.get());
    auto * shard_pool = shard_cache.bind_tensor(sharded, *index.find(sharded->name));
    REQUIRE(shard_pool && shard_pool->data);
    const int32_t shard_expert = 3;
    int32_t shard_slot = -1;
    shard_cache.resolve(&shard_expert, 1, &shard_slot);
    const auto * values = reinterpret_cast<const float *>(
        static_cast<const uint8_t *>(shard_pool->data) + shard_slot * shard_pool->nb[2]);
    for (int i = 0; i < 4; ++i) {
        REQUIRE(values[i] == static_cast<float>(130 + i));
    }
    REQUIRE(shard_cache.get_stats().snapshot().bytes_read == sharded->nb[2]);
    }

    bool small_budget_rejected = false;
    try {
        llama_moe_paging_params params{};
        params.mode = LLAMA_MOE_PAGING_MODE_EXPLICIT;
        params.io_mode = LLAMA_MOE_IO_MODE_BUFFERED;
        params.cache_bytes = 1;
        params.io_threads = 1;
        params.io_timeout_ms = 30000;
        llama_moe::paging_manager manager(backend.get(), params, index, 2);
        manager.bind_pool(0, original);
    } catch (const std::runtime_error &) {
        small_budget_rejected = true;
    }
    REQUIRE(small_budget_rejected);

    bool bad_range_rejected = false;
    try {
        llama_moe::model_index bad_index;
        bad_index.add_tensor(original->name, 0, file_bytes.size(), original);
        bad_index.open_files(files);
    } catch (const std::runtime_error &) {
        bad_range_rejected = true;
    }
    REQUIRE(bad_range_rejected);

    llama_moe_paging_params safe_params{};
    safe_params.mode = LLAMA_MOE_PAGING_MODE_EXPLICIT;
    safe_params.io_mode = LLAMA_MOE_IO_MODE_BUFFERED;
    safe_params.n_slots = 4;
    safe_params.io_threads = 1;
    safe_params.io_timeout_ms = 30000;
    llama_moe::paging_manager safe_manager(backend.get(), safe_params, index, 2);
    REQUIRE(safe_manager.max_safe_ubatch() == 2);

    backend.reset();
    llama_backend_free();
    files.clear();
    std::remove(path);
    std::remove(shard_path);
    return 0;
}
