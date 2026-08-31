// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright (c) 2026 SparkMoE contributors.
#pragma once
#include <cstdint>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#else
#include <fstream>
#include <sstream>
#include <string>
#endif

struct sparkmoe_process_memory {
    uint64_t rss = 0, peak_rss = 0, private_bytes = 0;
};

inline sparkmoe_process_memory sparkmoe_memory_snapshot() {
    sparkmoe_process_memory result;
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&counters), sizeof(counters))) {
        result.rss = counters.WorkingSetSize;
        result.peak_rss = counters.PeakWorkingSetSize;
        result.private_bytes = counters.PrivateUsage;
    }
#elif defined(__linux__)
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        std::istringstream row(line);
        std::string key;
        uint64_t kib = 0;
        if (!(row >> key >> kib)) { continue; }
        if (key == "VmRSS:") { result.rss = kib * 1024; }
        if (key == "VmHWM:") { result.peak_rss = kib * 1024; }
        if (key == "RssAnon:") { result.private_bytes = kib * 1024; }
    }
#endif
    return result;
}
