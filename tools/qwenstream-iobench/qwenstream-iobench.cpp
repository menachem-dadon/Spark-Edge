#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <io.h>
#endif

namespace {

using clock_type = std::chrono::steady_clock;
using file_ptr = std::unique_ptr<FILE, decltype(&std::fclose)>;

struct options {
    std::string path = "qwenstream-iobench.tmp";
    uint64_t size_mib = 64;
    uint64_t sequential_block_kib = 1024;
    uint64_t random_block_kib = 4;
    uint64_t random_reads = 4096;
    std::string format = "text";
    bool keep = false;
    bool custom_path = false;
};

struct results {
    double write_mib_s = 0;
    double sequential_read_mib_s = 0;
    double random_read_iops = 0;
    double random_p50_us = 0;
    double random_p95_us = 0;
    uint64_t bytes_written = 0;
    uint64_t bytes_read = 0;
};

[[noreturn]] void fail(const std::string & message) {
    throw std::runtime_error(message);
}

uint64_t parse_positive(const char * value, const char * name) {
    char * end = nullptr;
    const unsigned long long parsed = std::strtoull(value, &end, 10);
    if (!value[0] || !end || *end != '\0' || parsed == 0) {
        fail(std::string("invalid value for ") + name);
    }
    return parsed;
}

void usage(const char * program) {
    std::printf("Usage: %s [options]\n", program);
    std::printf("  --file PATH                  benchmark file (default: ./qwenstream-iobench.tmp)\n");
    std::printf("  --size-mib N                 file size in MiB (default: 64)\n");
    std::printf("  --sequential-block-kib N     sequential block size (default: 1024)\n");
    std::printf("  --random-block-kib N         random block size (default: 4)\n");
    std::printf("  --random-reads N             number of random reads (default: 4096)\n");
    std::printf("  --format text|json|csv       output format (default: text)\n");
    std::printf("  --keep                       keep a default temporary benchmark file\n");
}

options parse_options(int argc, char ** argv) {
    options result;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&]() -> const char * {
            if (++i >= argc) {
                fail("missing value after " + arg);
            }
            return argv[i];
        };
        if (arg == "-h" || arg == "--help") {
            usage(argv[0]);
            std::exit(0);
        } else if (arg == "--file") {
            result.path = next();
            result.custom_path = true;
        } else if (arg == "--size-mib") {
            result.size_mib = parse_positive(next(), "--size-mib");
        } else if (arg == "--sequential-block-kib") {
            result.sequential_block_kib = parse_positive(next(), "--sequential-block-kib");
        } else if (arg == "--random-block-kib") {
            result.random_block_kib = parse_positive(next(), "--random-block-kib");
        } else if (arg == "--random-reads") {
            result.random_reads = parse_positive(next(), "--random-reads");
        } else if (arg == "--format") {
            result.format = next();
            if (result.format != "text" && result.format != "json" && result.format != "csv") {
                fail("--format must be text, json, or csv");
            }
        } else if (arg == "--keep") {
            result.keep = true;
        } else {
            fail("unknown option: " + arg);
        }
    }
    return result;
}

file_ptr open_file(const std::string & path, const char * mode) {
#if defined(_WIN32)
    const int path_count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.c_str(), -1, nullptr, 0);
    const int mode_count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, mode, -1, nullptr, 0);
    if (path_count <= 0 || mode_count <= 0) {
        fail("file path is not valid UTF-8");
    }
    std::wstring wide_path(static_cast<size_t>(path_count), L'\0');
    std::wstring wide_mode(static_cast<size_t>(mode_count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.c_str(), -1, wide_path.data(), path_count);
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, mode, -1, wide_mode.data(), mode_count);
    FILE * raw = _wfopen(wide_path.c_str(), wide_mode.c_str());
#else
    FILE * raw = std::fopen(path.c_str(), mode);
#endif
    if (!raw) {
        fail("cannot open benchmark file: " + path);
    }
    return file_ptr(raw, &std::fclose);
}

void seek_file(FILE * file, uint64_t offset) {
#if defined(_WIN32)
    if (_fseeki64(file, static_cast<__int64>(offset), SEEK_SET) != 0) {
#else
    if (fseeko(file, static_cast<off_t>(offset), SEEK_SET) != 0) {
#endif
        fail("seek failed");
    }
}

double seconds_since(clock_type::time_point started) {
    return std::chrono::duration<double>(clock_type::now() - started).count();
}

double mib_per_second(uint64_t bytes, double seconds) {
    return seconds > 0 ? (static_cast<double>(bytes) / (1024.0 * 1024.0)) / seconds : 0;
}

results run(const options & opts) {
    const uint64_t file_bytes = opts.size_mib * 1024ULL * 1024ULL;
    const size_t sequential_block = static_cast<size_t>(opts.sequential_block_kib * 1024ULL);
    const size_t random_block = static_cast<size_t>(opts.random_block_kib * 1024ULL);
    if (random_block > file_bytes || sequential_block > file_bytes) {
        fail("block size cannot exceed benchmark file size");
    }

    std::vector<unsigned char> buffer(std::max(sequential_block, random_block));
    for (size_t i = 0; i < buffer.size(); ++i) {
        buffer[i] = static_cast<unsigned char>((i * 131U + 17U) & 0xffU);
    }

    results out;
    {
        auto file = open_file(opts.path, "w+b");
        const auto started = clock_type::now();
        while (out.bytes_written < file_bytes) {
            const size_t amount = static_cast<size_t>(std::min<uint64_t>(sequential_block, file_bytes - out.bytes_written));
            if (std::fwrite(buffer.data(), 1, amount, file.get()) != amount) {
                fail("write failed before the requested file size was reached");
            }
            out.bytes_written += amount;
        }
        if (std::fflush(file.get()) != 0) {
            fail("flush failed");
        }
#if defined(_WIN32)
        if (!FlushFileBuffers(reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(file.get()))))) {
            fail("FlushFileBuffers failed");
        }
#endif
        out.write_mib_s = mib_per_second(out.bytes_written, seconds_since(started));
    }

    {
        auto file = open_file(opts.path, "rb");
        const auto started = clock_type::now();
        while (out.bytes_read < file_bytes) {
            const size_t amount = static_cast<size_t>(std::min<uint64_t>(sequential_block, file_bytes - out.bytes_read));
            if (std::fread(buffer.data(), 1, amount, file.get()) != amount) {
                fail("sequential read failed");
            }
            out.bytes_read += amount;
        }
        out.sequential_read_mib_s = mib_per_second(out.bytes_read, seconds_since(started));
    }

    std::vector<double> latencies;
    latencies.reserve(static_cast<size_t>(opts.random_reads));
    {
        auto file = open_file(opts.path, "rb");
        const uint64_t blocks = file_bytes / random_block;
        std::mt19937_64 generator(0x5157454e5354524dULL);
        std::uniform_int_distribution<uint64_t> distribution(0, blocks - 1);
        const auto all_started = clock_type::now();
        for (uint64_t i = 0; i < opts.random_reads; ++i) {
            seek_file(file.get(), distribution(generator) * random_block);
            const auto started = clock_type::now();
            if (std::fread(buffer.data(), 1, random_block, file.get()) != random_block) {
                fail("random read failed");
            }
            latencies.push_back(std::chrono::duration<double, std::micro>(clock_type::now() - started).count());
        }
        const double elapsed = seconds_since(all_started);
        out.random_read_iops = elapsed > 0 ? static_cast<double>(opts.random_reads) / elapsed : 0;
    }

    std::sort(latencies.begin(), latencies.end());
    const auto percentile = [&](double value) {
        const size_t index = static_cast<size_t>((latencies.size() - 1) * value);
        return latencies[index];
    };
    out.random_p50_us = percentile(0.50);
    out.random_p95_us = percentile(0.95);
    return out;
}

void print_results(const options & opts, const results & value) {
    if (opts.format == "json") {
        std::printf("{\"file_size_mib\":%llu,\"sequential_block_kib\":%llu,\"random_block_kib\":%llu,"
                    "\"random_reads\":%llu,\"write_mib_s\":%.3f,\"sequential_read_mib_s\":%.3f,"
                    "\"random_read_iops\":%.3f,\"random_p50_us\":%.3f,\"random_p95_us\":%.3f}\n",
            static_cast<unsigned long long>(opts.size_mib),
            static_cast<unsigned long long>(opts.sequential_block_kib),
            static_cast<unsigned long long>(opts.random_block_kib),
            static_cast<unsigned long long>(opts.random_reads),
            value.write_mib_s, value.sequential_read_mib_s, value.random_read_iops,
            value.random_p50_us, value.random_p95_us);
    } else if (opts.format == "csv") {
        std::printf("file_size_mib,sequential_block_kib,random_block_kib,random_reads,write_mib_s,sequential_read_mib_s,random_read_iops,random_p50_us,random_p95_us\n");
        std::printf("%llu,%llu,%llu,%llu,%.3f,%.3f,%.3f,%.3f,%.3f\n",
            static_cast<unsigned long long>(opts.size_mib),
            static_cast<unsigned long long>(opts.sequential_block_kib),
            static_cast<unsigned long long>(opts.random_block_kib),
            static_cast<unsigned long long>(opts.random_reads),
            value.write_mib_s, value.sequential_read_mib_s, value.random_read_iops,
            value.random_p50_us, value.random_p95_us);
    } else {
        std::printf("QwenStream buffered file I/O benchmark\n");
        std::printf("File size:              %llu MiB\n", static_cast<unsigned long long>(opts.size_mib));
        std::printf("Write:                  %.3f MiB/s\n", value.write_mib_s);
        std::printf("Sequential read:        %.3f MiB/s\n", value.sequential_read_mib_s);
        std::printf("Random read:            %.3f IOPS\n", value.random_read_iops);
        std::printf("Random read latency:    p50 %.3f us, p95 %.3f us\n", value.random_p50_us, value.random_p95_us);
        std::printf("Note: OS caching may affect these buffered-I/O measurements.\n");
    }
}

void remove_default_file(const options & opts) {
    if (!opts.custom_path && !opts.keep) {
#if defined(_WIN32)
        const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, opts.path.c_str(), -1, nullptr, 0);
        if (count > 0) {
            std::wstring path(static_cast<size_t>(count), L'\0');
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, opts.path.c_str(), -1, path.data(), count);
            _wremove(path.c_str());
        }
#else
        std::remove(opts.path.c_str());
#endif
    }
}

} // namespace

int main(int argc, char ** argv) {
    try {
        const options opts = parse_options(argc, argv);
        const results value = run(opts);
        print_results(opts, value);
        remove_default_file(opts);
        return 0;
    } catch (const std::exception & error) {
        std::fprintf(stderr, "qwenstream-iobench: %s\n", error.what());
        return 1;
    }
}
