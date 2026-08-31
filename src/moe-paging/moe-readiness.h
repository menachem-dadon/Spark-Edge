// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright (c) 2026 SparkMoE contributors.
#pragma once
#include "moe-io.h"
#include "moe-stats.h"
#include "ggml.h"
#include <mutex>
#include <vector>

namespace llama_moe {
class read_ticket;
struct read_batch {
    std::atomic<bool> observed{false};
    std::vector<std::weak_ptr<read_ticket>> reads;
};
class read_ticket {
public:
    read_ticket(std::shared_ptr<io_executor> executor, std::shared_ptr<stats> counters,
                std::function<io_result()> read, bool speculative);
    ~read_ticket();
    bool ready() const;
    const io_result & finish(bool required, bool compute = false);
    void promote();
    void cancel();
    void set_batch(std::shared_ptr<read_batch> batch) { this->batch = std::move(batch); }
private:
    std::shared_ptr<read_batch> batch;
    std::shared_ptr<io_executor> executor;
    std::shared_ptr<stats> counters;
    std::function<io_result()> read;
    std::shared_future<io_result> future;
    uint64_t tag;
    bool speculative;
    bool retried = false;
    io_result result, retry_result;
    std::once_flag collect_once, retry_once, compute_once;
};

struct readiness_table {
    explicit readiness_table(size_t slots) : reads(slots) {}
    std::mutex mutex;
    std::vector<std::shared_ptr<read_ticket>> reads;
};

void register_readiness(const ggml_tensor * tensor, std::shared_ptr<readiness_table> table);
void unregister_readiness(const ggml_tensor * tensor);
}
