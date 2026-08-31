// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright (c) 2026 SparkMoE contributors.
#pragma once
#include <cstddef>
#include <cstdint>

namespace llama_moe {
size_t memory_page_size();
uint64_t page_aligned(uint64_t bytes);

class virtual_memory {
public:
    explicit virtual_memory(uint64_t bytes);
    ~virtual_memory();
    virtual_memory(const virtual_memory &) = delete;
    virtual_memory & operator=(const virtual_memory &) = delete;
    void commit(uint64_t offset, uint64_t bytes);
    void discard(uint64_t offset, uint64_t bytes);
    void * data() const { return address; }
    size_t size() const { return length; }
private:
    void validate(uint64_t offset, uint64_t bytes) const;
    void * address = nullptr;
    size_t length = 0;
};
}
