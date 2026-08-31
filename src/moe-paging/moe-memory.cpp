// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright (c) 2026 SparkMoE contributors.
#include "moe-memory.h"
#include <limits>
#include <stdexcept>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace llama_moe {
size_t memory_page_size() {
    static const size_t page = [] {
#if defined(_WIN32)
        SYSTEM_INFO info{};
        GetSystemInfo(&info);
        return static_cast<size_t>(info.dwPageSize);
#else
        const long result = sysconf(_SC_PAGESIZE);
        if (result <= 0) { throw std::runtime_error("cannot determine system page size"); }
        return static_cast<size_t>(result);
#endif
    }();
    return page;
}

uint64_t page_aligned(uint64_t bytes) {
    const uint64_t page = memory_page_size();
    if (!page || bytes > UINT64_MAX - page + 1) { throw std::overflow_error("expert allocation size overflow"); }
    return (bytes + page - 1) / page * page;
}

virtual_memory::virtual_memory(uint64_t bytes) {
    bytes = page_aligned(bytes);
    if (!bytes || bytes > std::numeric_limits<size_t>::max()) { throw std::invalid_argument("invalid virtual pool size"); }
    length = static_cast<size_t>(bytes);
#if defined(_WIN32)
    address = VirtualAlloc(nullptr, length, MEM_RESERVE, PAGE_NOACCESS);
#else
    address = mmap(nullptr, length, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (address == MAP_FAILED) { address = nullptr; }
#endif
    if (!address) { throw std::runtime_error("cannot reserve virtual expert pool"); }
}

virtual_memory::~virtual_memory() {
#if defined(_WIN32)
    if (address) { VirtualFree(address, 0, MEM_RELEASE); }
#else
    if (address) { munmap(address, length); }
#endif
}

void virtual_memory::validate(uint64_t offset, uint64_t bytes) const {
    if (offset % memory_page_size() || bytes % memory_page_size() || offset > length || bytes > length - offset) {
        throw std::invalid_argument("unaligned or out-of-range virtual pool operation");
    }
}

void virtual_memory::commit(uint64_t offset, uint64_t bytes) {
    validate(offset, bytes);
    auto * start = static_cast<uint8_t *>(address) + offset;
#if defined(_WIN32)
    if (!VirtualAlloc(start, static_cast<size_t>(bytes), MEM_COMMIT, PAGE_READWRITE)) {
#else
    if (mprotect(start, static_cast<size_t>(bytes), PROT_READ | PROT_WRITE) != 0) {
#endif
        throw std::runtime_error("cannot commit expert pool pages");
    }
}

void virtual_memory::discard(uint64_t offset, uint64_t bytes) {
    validate(offset, bytes);
    auto * start = static_cast<uint8_t *>(address) + offset;
#if defined(_WIN32)
    if (!VirtualFree(start, static_cast<size_t>(bytes), MEM_DECOMMIT)) {
#else
    if (madvise(start, static_cast<size_t>(bytes), MADV_DONTNEED) != 0 ||
            mprotect(start, static_cast<size_t>(bytes), PROT_NONE) != 0) {
#endif
        throw std::runtime_error("cannot discard expert pool pages");
    }
}
}
