#include "ggml-backend.h"
#include "ggml-cpp.h"
#include "llama-mmap.h"
#include "llama.h"
#include "moe-paging/moe-cache.h"
#include "moe-paging/moe-model-index.h"

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

    ggml_init_params metadata_params{};
    metadata_params.mem_size = ggml_tensor_overhead() * 4;
    metadata_params.no_alloc = true;
    ggml_context_ptr metadata(ggml_init(metadata_params));
    REQUIRE(metadata);

    auto * original = ggml_new_tensor_3d(metadata.get(), GGML_TYPE_F32, 2, 2, 4);
    ggml_set_name(original, "blk.0.ffn_up_exps.weight");
    REQUIRE(original->nb[2] == 4 * sizeof(float));

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

    llama_moe::model_index index;
    index.add_tensor(original->name, 0, 64, original);
    std::vector<std::unique_ptr<llama_file>> files;
    files.push_back(std::make_unique<llama_file>(path, "rb"));
    index.open_files(files);

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
        for (int i = 0; i < 4; ++i) {
            REQUIRE(values[i] == static_cast<float>(first_ids[request] * 10 + i));
        }
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
    }

    backend.reset();
    llama_backend_free();
    files.clear();
    std::remove(path);
    return 0;
}
