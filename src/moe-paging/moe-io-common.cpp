// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright (c) 2026 SparkMoE contributors.

#include "moe-io.h"

#include <condition_variable>
#include <algorithm>
#include <chrono>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace llama_moe {

struct io_executor::impl {
    explicit impl(uint32_t thread_count) {
        if (thread_count > 128) { throw std::invalid_argument("MoE I/O thread count exceeds 128"); }
        if (thread_count == 0) {
            thread_count = 1;
        }
        workers.reserve(thread_count);
        try {
            for (uint32_t i = 0; i < thread_count; ++i) { workers.emplace_back([this] { worker_loop(); }); }
        } catch (...) {
            { std::lock_guard<std::mutex> lock(mutex); stopping = true; }
            condition.notify_all();
            for (auto & worker : workers) { worker.join(); }
            throw;
        }
    }

    ~impl() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
        }
        condition.notify_all();
        for (auto & worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    std::future<io_result> submit(std::function<io_result()> task, io_priority priority, uint64_t tag) {
        const auto queued = std::chrono::steady_clock::now();
        auto packaged = std::make_shared<std::packaged_task<io_result()>>([task = std::move(task), queued] {
            try {
                const auto delay = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - queued).count();
                auto result = task();
                result.queue_us = static_cast<uint64_t>(delay);
                return result;
            }
            catch (const std::exception & error) {
                io_result result;
                result.error = error.what();
                return result;
            }
            catch (...) { io_result result; result.error = "unknown I/O exception"; return result; }
        });
        auto future = packaged->get_future();
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (stopping) {
                throw std::runtime_error("MoE I/O executor is stopping");
            }
            auto & queue = priority == io_priority::demand ? tasks : prefetch;
            if (priority == io_priority::prefetch && queue.size() >= 128) {
                std::promise<io_result> rejected;
                io_result result;
                result.error = "prefetch queue is full";
                rejected.set_value(result);
                return rejected.get_future();
            }
            queue.push_back({tag, [packaged] { (*packaged)(); }});
        }
        condition.notify_one();
        return future;
    }

    bool promote(uint64_t tag) {
        std::lock_guard<std::mutex> lock(mutex);
        const auto found = std::find_if(prefetch.begin(), prefetch.end(), [tag](const job & item) { return item.tag == tag; });
        if (found == prefetch.end()) { return false; }
        tasks.push_back(std::move(*found));
        prefetch.erase(found);
        condition.notify_one();
        return true;
    }

    bool cancel(uint64_t tag) {
        std::lock_guard<std::mutex> lock(mutex);
        const auto found = std::find_if(prefetch.begin(), prefetch.end(), [tag](const job & item) { return item.tag == tag; });
        if (found == prefetch.end()) { return false; }
        prefetch.erase(found); // A cancelled packaged_task completes its future with broken_promise.
        return true;
    }

    void worker_loop() {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex);
                condition.wait(lock, [this] { return stopping || !tasks.empty() || !prefetch.empty(); });
                if (stopping && tasks.empty() && prefetch.empty()) {
                    return;
                }
                auto & queue = tasks.empty() ? prefetch : tasks;
                task = std::move(queue.front().run);
                queue.pop_front();
            }
            task();
        }
    }

    std::mutex mutex;
    std::condition_variable condition;
    struct job { uint64_t tag; std::function<void()> run; };
    std::deque<job> tasks;
    std::deque<job> prefetch;
    std::vector<std::thread> workers;
    bool stopping = false;
};

io_executor::io_executor(uint32_t thread_count) : pimpl(std::make_unique<impl>(thread_count)) {}
io_executor::~io_executor() = default;

std::future<io_result> io_executor::submit(std::function<io_result()> task, io_priority priority, uint64_t tag) {
    return pimpl->submit(std::move(task), priority, tag);
}
bool io_executor::promote(uint64_t tag) { return pimpl->promote(tag); }
bool io_executor::cancel(uint64_t tag) { return pimpl->cancel(tag); }

}
