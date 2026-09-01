// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright (c) 2026 SparkMoE contributors.

#include "httplib.h"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#else
#include <csignal>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using json = nlohmann::ordered_json;

namespace {

constexpr int default_manager_port = 18080;
constexpr size_t max_log_read_bytes = 1024 * 1024;

int manager_port() {
    static const int port = []() {
        const char * value = std::getenv("SPARKMOE_MANAGER_PORT");
        if (!value) {
            return default_manager_port;
        }
        try {
            const int parsed = std::stoi(value);
            return parsed >= 1024 && parsed <= 65535 ? parsed : default_manager_port;
        } catch (...) {
            return default_manager_port;
        }
    }();
    return port;
}

struct router_config {
    std::string model_directory;
    int port = 8080;
    int models_max = 1;
    bool models_autoload = true;
};

struct model_profile {
    std::string mode = "auto";
    int cache = 2048;
    int ple_cache_mib = 64;
    int slots = 0;
    int io_threads = 4;
    std::string io_mode = "buffered";
    std::string cache_policy = "lru";
    std::string overlap = "off";
    std::string prefetch = "off";
    std::string dense_residency = "auto";
    int paged_layers = 0;
    int inference_threads = 0;
    std::string cpu_repack = "auto";
    int ctx_size = 8192;
    int batch_size = 1024;
    int ubatch_size = 128;
    int parallel = 1;
    int ctx_checkpoints = 4;
    int cache_ram_mib = 0;
    std::string cache_type_k = "f16";
    std::string cache_type_v = "f16";
    bool context_shift = true;
    std::string reasoning = "auto";
    bool mtp_enabled = false;
    std::string mtp_model;
    int mtp_cache_mib = 512;
    int mtp_draft_tokens = 3;
    int mtp_min_tokens = 0;
    double mtp_p_min = 0.0;
    int mtp_threads = 0;
    bool mtp_backend_sampling = true;
};

struct app_state {
    std::mutex mutex;
    std::mutex log_mutex;
    router_config router;
    model_profile default_profile;
    std::map<std::string, model_profile> profiles;
    std::filesystem::path data_directory;
    std::filesystem::path config_path;
    std::filesystem::path preset_path;
    std::filesystem::path logs_directory;
    std::filesystem::path log_path;
    std::string log_session;
    std::string last_error;
    int previous_router_port = 0;
    int64_t started_at_ms = 0;
    int last_exit_code = 0;
    bool has_exit_code = false;
    std::thread log_thread;
#if defined(_WIN32)
    HANDLE process = nullptr;
    HANDLE job = nullptr;
    DWORD pid = 0;
#else
    pid_t pid = -1;
#endif
};

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string timestamp_string(bool filename) {
    const auto now = std::chrono::system_clock::now();
    const std::time_t value = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &value);
#else
    localtime_r(&value, &tm);
#endif
    std::ostringstream output;
    output << std::put_time(&tm, filename ? "%Y%m%d-%H%M%S" : "%Y-%m-%d %H:%M:%S");
    return output.str();
}

#if defined(_WIN32)
std::wstring utf8_to_wide(const std::string & value) {
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.c_str(), -1, nullptr, 0);
    if (count <= 0) {
        throw std::runtime_error("text is not valid UTF-8");
    }
    std::wstring result(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.c_str(), -1, result.data(), count);
    result.resize(static_cast<size_t>(count - 1));
    return result;
}

std::string wide_to_utf8(const std::wstring & value) {
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (count <= 0) {
        throw std::runtime_error("path cannot be converted to UTF-8");
    }
    std::string result(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.c_str(), -1, result.data(), count, nullptr, nullptr);
    result.resize(static_cast<size_t>(count - 1));
    return result;
}

std::wstring quote_windows_arg(const std::wstring & value) {
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (wchar_t c : value) {
        if (c == L'\\') {
            ++slashes;
        } else {
            if (c == L'\"') {
                result.append(slashes * 2 + 1, L'\\');
            } else {
                result.append(slashes, L'\\');
            }
            slashes = 0;
            result.push_back(c);
        }
    }
    result.append(slashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}

std::filesystem::path executable_directory() {
    std::vector<wchar_t> path(32768);
    const DWORD count = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (count == 0 || count >= path.size()) {
        throw std::runtime_error("cannot determine launcher directory");
    }
    return std::filesystem::path(std::wstring(path.data(), count)).parent_path();
}

std::filesystem::path local_data_directory() {
    const wchar_t * override_path = _wgetenv(L"SPARKMOE_DATA_DIR");
    if (override_path && *override_path) {
        return std::filesystem::path(override_path);
    }
    PWSTR path = nullptr;
    const HRESULT result = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &path);
    if (FAILED(result) || !path) {
        if (path) {
            CoTaskMemFree(path);
        }
        throw std::runtime_error("cannot locate the local application data directory");
    }
    const std::filesystem::path directory = std::filesystem::path(path) / L"SparkMoE";
    CoTaskMemFree(path);
    return directory;
}
#else
std::filesystem::path executable_directory() {
    std::error_code error;
    const auto path = std::filesystem::read_symlink("/proc/self/exe", error);
    return error ? std::filesystem::current_path() : path.parent_path();
}

std::filesystem::path local_data_directory() {
    const char * override_path = std::getenv("SPARKMOE_DATA_DIR");
    if (override_path && *override_path) {
        return std::filesystem::path(override_path);
    }
    const char * xdg = std::getenv("XDG_STATE_HOME");
    if (xdg && *xdg) {
        return std::filesystem::path(xdg) / "SparkMoE";
    }
    const char * user_home = std::getenv("HOME");
    return user_home && *user_home ? std::filesystem::path(user_home) / ".local" / "state" / "SparkMoE" :
        std::filesystem::current_path() / ".sparkmoe";
}
#endif

std::string utf8_path(const std::filesystem::path & path) {
#if defined(_WIN32)
    return wide_to_utf8(path.wstring());
#else
    return path.string();
#endif
}

std::filesystem::path native_path(const std::string & path) {
#if defined(_WIN32)
    return std::filesystem::path(utf8_to_wide(path));
#else
    return std::filesystem::path(path);
#endif
}

void append_log(app_state & state, const std::string & message) {
    std::lock_guard<std::mutex> lock(state.log_mutex);
    if (state.log_path.empty()) {
        return;
    }
    std::ofstream output(state.log_path, std::ios::binary | std::ios::app);
    if (output) {
        output << '[' << timestamp_string(false) << "] SparkMoE: " << message << '\n';
    }
}

json router_config_json(const router_config & config) {
    return {
        {"model_directory", config.model_directory},
        {"port", config.port},
        {"models_max", config.models_max},
        {"models_autoload", config.models_autoload},
    };
}

router_config router_config_from_json(const json & input, const router_config & base) {
    router_config config = base;
    config.model_directory = input.value("model_directory", config.model_directory);
    config.port = input.value("port", config.port);
    config.models_max = input.value("models_max", config.models_max);
    config.models_autoload = input.value("models_autoload", config.models_autoload);
    return config;
}

json profile_json(const model_profile & profile) {
    return {
        {"mode", profile.mode},
        {"cache", profile.cache},
        {"ple_cache_mib", profile.ple_cache_mib},
        {"slots", profile.slots},
        {"io_threads", profile.io_threads},
        {"io_mode", profile.io_mode},
        {"cache_policy", profile.cache_policy},
        {"overlap", profile.overlap},
        {"prefetch", profile.prefetch},
        {"dense_residency", profile.dense_residency},
        {"paged_layers", profile.paged_layers},
        {"inference_threads", profile.inference_threads},
        {"cpu_repack", profile.cpu_repack},
        {"ctx_size", profile.ctx_size},
        {"batch_size", profile.batch_size},
        {"ubatch_size", profile.ubatch_size},
        {"parallel", profile.parallel},
        {"ctx_checkpoints", profile.ctx_checkpoints},
        {"cache_ram_mib", profile.cache_ram_mib},
        {"cache_type_k", profile.cache_type_k},
        {"cache_type_v", profile.cache_type_v},
        {"context_shift", profile.context_shift},
        {"reasoning", profile.reasoning},
        {"mtp_enabled", profile.mtp_enabled},
        {"mtp_model", profile.mtp_model},
        {"mtp_cache_mib", profile.mtp_cache_mib},
        {"mtp_draft_tokens", profile.mtp_draft_tokens},
        {"mtp_min_tokens", profile.mtp_min_tokens},
        {"mtp_p_min", profile.mtp_p_min},
        {"mtp_threads", profile.mtp_threads},
        {"mtp_backend_sampling", profile.mtp_backend_sampling},
    };
}

model_profile profile_from_json(const json & input, const model_profile & base) {
    model_profile profile = base;
    profile.mode = input.value("mode", profile.mode);
    profile.cache = input.value("cache", profile.cache);
    profile.ple_cache_mib = input.value("ple_cache_mib", profile.ple_cache_mib);
    profile.slots = input.value("slots", profile.slots);
    profile.io_threads = input.value("io_threads", profile.io_threads);
    profile.io_mode = input.value("io_mode", profile.io_mode);
    profile.cache_policy = input.value("cache_policy", profile.cache_policy);
    profile.overlap = input.value("overlap", profile.overlap);
    profile.prefetch = input.value("prefetch", profile.prefetch);
    profile.dense_residency = input.value("dense_residency", profile.dense_residency);
    profile.paged_layers = input.value("paged_layers", profile.paged_layers);
    profile.inference_threads = input.value("inference_threads", profile.inference_threads);
    profile.cpu_repack = input.value("cpu_repack", profile.cpu_repack);
    profile.ctx_size = input.value("ctx_size", profile.ctx_size);
    profile.batch_size = input.value("batch_size", profile.batch_size);
    profile.ubatch_size = input.value("ubatch_size", profile.ubatch_size);
    profile.parallel = input.value("parallel", profile.parallel);
    profile.ctx_checkpoints = input.value("ctx_checkpoints", profile.ctx_checkpoints);
    profile.cache_ram_mib = input.value("cache_ram_mib", profile.cache_ram_mib);
    profile.cache_type_k = input.value("cache_type_k", profile.cache_type_k);
    profile.cache_type_v = input.value("cache_type_v", profile.cache_type_v);
    profile.context_shift = input.value("context_shift", profile.context_shift);
    profile.reasoning = input.value("reasoning", profile.reasoning);
    profile.mtp_enabled = input.value("mtp_enabled", profile.mtp_enabled);
    profile.mtp_model = input.value("mtp_model", profile.mtp_model);
    profile.mtp_cache_mib = input.value("mtp_cache_mib", profile.mtp_cache_mib);
    profile.mtp_draft_tokens = input.value("mtp_draft_tokens", profile.mtp_draft_tokens);
    profile.mtp_min_tokens = input.value("mtp_min_tokens", profile.mtp_min_tokens);
    profile.mtp_p_min = input.value("mtp_p_min", profile.mtp_p_min);
    profile.mtp_threads = input.value("mtp_threads", profile.mtp_threads);
    profile.mtp_backend_sampling = input.value("mtp_backend_sampling", profile.mtp_backend_sampling);
    return profile;
}

void validate_router_config(const router_config & config) {
    if (config.port < 1024 || config.port > 65535 || config.port == manager_port()) {
        throw std::runtime_error("server port must be 1024..65535 and cannot match the manager port");
    }
    if (config.models_max < 0 || config.models_max > 64) {
        throw std::runtime_error("models-max must be between 0 and 64");
    }
    if (!config.model_directory.empty()) {
        std::error_code error;
        if (!std::filesystem::is_directory(native_path(config.model_directory), error) || error) {
            throw std::runtime_error("models directory does not exist or is not readable");
        }
    }
}

bool local_gguf_is_valid(const std::string & path) {
    if (path.empty()) {
        return true;
    }
    const std::string lower = lowercase(path);
    if (lower.size() < 5 || lower.substr(lower.size() - 5) != ".gguf") {
        return false;
    }
    std::error_code error;
    return std::filesystem::is_regular_file(native_path(path), error) && !error;
}

void validate_profile(const model_profile & profile) {
    static const std::unordered_set<std::string> cache_types = {
        "f16", "bf16", "q8_0", "q4_0", "q4_1", "iq4_nl", "q5_0", "q5_1",
    };
    if (profile.mode != "explicit" && profile.mode != "auto" && profile.mode != "os" && profile.mode != "off") {
        throw std::runtime_error("invalid paging mode");
    }
    if (profile.cache < 0 || profile.slots < 0 || profile.io_threads < 1 || profile.io_threads > 128 ||
            profile.ple_cache_mib < 1 || profile.ple_cache_mib > 4096 || profile.inference_threads < 0 ||
            profile.inference_threads > 128 || profile.paged_layers < 0 || profile.paged_layers > 4096) {
        throw std::runtime_error("invalid loading profile");
    }
    if ((profile.mode == "explicit" || profile.mode == "auto") && profile.cache == 0 && profile.slots == 0) {
        throw std::runtime_error("explicit or auto mode requires a cache budget or slot count");
    }
    if ((profile.mode == "explicit" || profile.mode == "auto") && profile.parallel != 1) {
        throw std::runtime_error("expert paging currently requires one parallel chat slot");
    }
    if (profile.io_mode != "buffered" && profile.io_mode != "direct") {
        throw std::runtime_error("invalid expert storage I/O mode");
    }
    if (profile.cache_policy != "lru" && profile.cache_policy != "hybrid") {
        throw std::runtime_error("invalid expert cache policy");
    }
    if ((profile.overlap != "on" && profile.overlap != "off") ||
            (profile.prefetch != "off" && profile.prefetch != "temporal")) {
        throw std::runtime_error("invalid overlap or prefetch policy");
    }
    if (profile.dense_residency != "auto" && profile.dense_residency != "mmap" &&
            profile.dense_residency != "resident" && profile.dense_residency != "locked") {
        throw std::runtime_error("invalid dense weight policy");
    }
    if (profile.cpu_repack != "auto" && profile.cpu_repack != "on" && profile.cpu_repack != "off") {
        throw std::runtime_error("invalid CPU repack mode");
    }
    if (profile.ctx_size < 0 || profile.ctx_size > 1048576 || profile.batch_size < 1 ||
            profile.batch_size > 16384 || profile.ubatch_size < 1 || profile.ubatch_size > profile.batch_size ||
            profile.parallel < 1 || profile.parallel > 32 || profile.ctx_checkpoints < 0 ||
            profile.ctx_checkpoints > 128 || profile.cache_ram_mib < -1 || profile.cache_ram_mib > 1048576) {
        throw std::runtime_error("invalid context profile");
    }
    if (cache_types.count(profile.cache_type_k) == 0 || cache_types.count(profile.cache_type_v) == 0) {
        throw std::runtime_error("unsupported KV cache type");
    }
    if (profile.reasoning != "auto" && profile.reasoning != "on" && profile.reasoning != "off") {
        throw std::runtime_error("reasoning must be auto, on, or off");
    }
    if (!local_gguf_is_valid(profile.mtp_model)) {
        throw std::runtime_error("MTP sidecar must be a readable local .gguf file");
    }
    if (profile.mtp_cache_mib < 1 || profile.mtp_cache_mib > 65536 ||
            profile.mtp_draft_tokens < 1 || profile.mtp_draft_tokens > 64 || profile.mtp_min_tokens < 0 ||
            profile.mtp_min_tokens > profile.mtp_draft_tokens || profile.mtp_p_min < 0.0 ||
            profile.mtp_p_min > 1.0 || profile.mtp_threads < 0 || profile.mtp_threads > 128) {
        throw std::runtime_error("invalid MTP profile");
    }
}

void validate_model_id(const std::string & model) {
    if (model.empty() || model == "*" || model.find(']') != std::string::npos ||
            model.find('\n') != std::string::npos || model.find('\r') != std::string::npos) {
        throw std::runtime_error("invalid model identifier");
    }
}

void validate_ini_value(const std::string & value) {
    if (value.find('\n') != std::string::npos || value.find('\r') != std::string::npos) {
        throw std::runtime_error("profile paths cannot contain line breaks");
    }
}

void write_profile(std::ostream & output, const model_profile & profile) {
    validate_ini_value(profile.mtp_model);
    output << "ctx-size = " << profile.ctx_size << '\n';
    output << "batch-size = " << profile.batch_size << '\n';
    output << "ubatch-size = " << profile.ubatch_size << '\n';
    output << "parallel = " << profile.parallel << '\n';
    output << "ctx-checkpoints = " << profile.ctx_checkpoints << '\n';
    output << "cache-ram = " << profile.cache_ram_mib << '\n';
    output << "cache-type-k = " << profile.cache_type_k << '\n';
    output << "cache-type-v = " << profile.cache_type_v << '\n';
    output << "context-shift = " << (profile.context_shift ? "true" : "false") << '\n';
    output << "reasoning = " << profile.reasoning << '\n';
    output << "moe-paging = " << profile.mode << '\n';
    if (profile.cache > 0) {
        output << "moe-cache-mib = " << profile.cache << '\n';
    }
    if (profile.slots > 0) {
        output << "moe-slots = " << profile.slots << '\n';
    }
    output << "moe-io-threads = " << profile.io_threads << '\n';
    output << "moe-ple-cache-mib = " << profile.ple_cache_mib << '\n';
    output << "moe-io-mode = " << profile.io_mode << '\n';
    output << "moe-cache-policy = " << profile.cache_policy << '\n';
    output << "moe-overlap = " << profile.overlap << '\n';
    output << "moe-prefetch = " << profile.prefetch << '\n';
    output << "moe-paged-layers = " << profile.paged_layers << '\n';
    output << "moe-dense-residency = " <<
        (profile.mode == "off" && profile.dense_residency == "auto" ? "resident" : profile.dense_residency) << '\n';
    if (profile.inference_threads > 0) {
        output << "threads = " << profile.inference_threads << '\n';
        output << "threads-batch = " << profile.inference_threads << '\n';
    }
    if (profile.cpu_repack == "on") {
        output << "repack = true\n";
    } else if (profile.cpu_repack == "off") {
        output << "repack = false\n";
    }
    output << "spec-type = " << (profile.mtp_enabled ? "draft-mtp" : "none") << '\n';
    if (profile.mtp_enabled) {
        if (!profile.mtp_model.empty()) {
            output << "model-draft = " << profile.mtp_model << '\n';
        }
        output << "moe-draft-cache-mib = " << profile.mtp_cache_mib << '\n';
        output << "spec-draft-n-max = " << profile.mtp_draft_tokens << '\n';
        output << "spec-draft-n-min = " << profile.mtp_min_tokens << '\n';
        output << "spec-draft-p-min = " << profile.mtp_p_min << '\n';
        output << "spec-draft-backend-sampling = " << (profile.mtp_backend_sampling ? "true" : "false") << '\n';
        if (profile.mtp_threads > 0) {
            output << "spec-draft-threads = " << profile.mtp_threads << '\n';
        }
    }
}

void write_preset_locked(app_state & state) {
    std::ofstream output(state.preset_path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot write the model profiles file");
    }
    output << "# Generated by SparkMoE. Edit profiles through the Web UI.\n\n[*]\n";
    output << "n-gpu-layers = 0\n";
    write_profile(output, state.default_profile);
    for (const auto & item : state.profiles) {
        validate_model_id(item.first);
        output << "\n[" << item.first << "]\n";
        write_profile(output, item.second);
    }
    if (!output) {
        throw std::runtime_error("cannot finish writing the model profiles file");
    }
}

void save_config_locked(app_state & state) {
    json profiles = json::object();
    for (const auto & item : state.profiles) {
        profiles[item.first] = profile_json(item.second);
    }
    const json root = {
        {"version", 1},
        {"router", router_config_json(state.router)},
        {"default_profile", profile_json(state.default_profile)},
        {"profiles", profiles},
    };
    std::ofstream output(state.config_path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot write the SparkMoE configuration");
    }
    output << root.dump(2) << '\n';
    if (!output) {
        throw std::runtime_error("cannot finish writing the SparkMoE configuration");
    }
    write_preset_locked(state);
}

void load_config(app_state & state) {
    if (!std::filesystem::exists(state.config_path)) {
        std::lock_guard<std::mutex> lock(state.mutex);
        save_config_locked(state);
        return;
    }
    std::ifstream input(state.config_path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot read the SparkMoE configuration");
    }
    const json root = json::parse(input);
    router_config router = router_config_from_json(root.value("router", json::object()), state.router);
    model_profile default_profile = profile_from_json(
        root.value("default_profile", json::object()), state.default_profile);
    std::map<std::string, model_profile> profiles;
    const json saved_profiles = root.value("profiles", json::object());
    if (!saved_profiles.is_object()) {
        throw std::runtime_error("saved model profiles are invalid");
    }
    validate_router_config(router);
    validate_profile(default_profile);
    for (const auto & item : saved_profiles.items()) {
        validate_model_id(item.key());
        model_profile profile = profile_from_json(item.value(), default_profile);
        validate_profile(profile);
        profiles[item.key()] = profile;
    }
    std::lock_guard<std::mutex> lock(state.mutex);
    state.router = router;
    state.default_profile = default_profile;
    state.profiles = std::move(profiles);
    write_preset_locked(state);
}

json recommendation_json() {
    uint64_t total_mib = 0;
    uint64_t available_mib = 0;
    uint32_t cpu_threads = 1;
#if defined(_WIN32)
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory)) {
        total_mib = memory.ullTotalPhys / (1024ULL * 1024ULL);
        available_mib = memory.ullAvailPhys / (1024ULL * 1024ULL);
    }
    cpu_threads = std::max<DWORD>(1, GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
#else
    const long pages = sysconf(_SC_PHYS_PAGES);
    const long page_size = sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && page_size > 0) {
        total_mib = static_cast<uint64_t>(pages) * static_cast<uint64_t>(page_size) / (1024ULL * 1024ULL);
    }
    const long available_pages = sysconf(_SC_AVPHYS_PAGES);
    if (available_pages > 0 && page_size > 0) {
        available_mib = static_cast<uint64_t>(available_pages) * static_cast<uint64_t>(page_size) / (1024ULL * 1024ULL);
    }
    const long online = sysconf(_SC_NPROCESSORS_ONLN);
    cpu_threads = online > 0 ? static_cast<uint32_t>(online) : 1;
#endif
    const uint64_t cache_mib = total_mib <= 8192 ? 1024 : total_mib <= 16384 ? 1536 :
        total_mib <= 32768 ? 4096 : std::min<uint64_t>(8192, total_mib / 8);
    const uint64_t ctx_size = total_mib <= 8192 ? 4096 : total_mib <= 16384 ? 8192 :
        total_mib <= 32768 ? 16384 : 32768;
    return {
        {"total_ram_mib", total_mib},
        {"available_ram_mib", available_mib},
        {"used_ram_mib", total_mib > available_mib ? total_mib - available_mib : 0},
        {"cpu_threads", cpu_threads},
        {"cache_mib", cache_mib},
        {"io_threads", std::max<uint32_t>(1, std::min<uint32_t>(4, cpu_threads / 4))},
        {"ctx_size", ctx_size},
        {"batch_size", 1024},
        {"ubatch_size", 128},
        {"reason", "Keeps RAM headroom and limits prompt batches for CPU expert paging."},
    };
}

void configure_initial_recommendation(app_state & state) {
    const json recommendation = recommendation_json();
    state.default_profile.cache = recommendation["cache_mib"].get<int>();
    state.default_profile.io_threads = recommendation["io_threads"].get<int>();
    state.default_profile.ctx_size = recommendation["ctx_size"].get<int>();
    state.default_profile.batch_size = recommendation["batch_size"].get<int>();
    state.default_profile.ubatch_size = recommendation["ubatch_size"].get<int>();
}

std::vector<std::string> router_arguments(const app_state & state) {
    std::vector<std::string> arguments = {
        "--host", "127.0.0.1",
        "--port", std::to_string(state.router.port),
        "--models-preset", utf8_path(state.preset_path),
        "--models-max", std::to_string(state.router.models_max),
        state.router.models_autoload ? "--models-autoload" : "--no-models-autoload",
        "--metrics",
        "--log-colors", "off",
        "--log-timestamps",
    };
    if (!state.router.model_directory.empty()) {
        arguments.insert(arguments.end(), {"--models-dir", state.router.model_directory});
    }
    return arguments;
}

void join_log_thread_locked(app_state & state) {
    if (state.log_thread.joinable()) {
        state.log_thread.join();
    }
}

bool process_running_locked(app_state & state) {
#if defined(_WIN32)
    if (!state.process) {
        return false;
    }
    DWORD code = 0;
    if (GetExitCodeProcess(state.process, &code) && code == STILL_ACTIVE) {
        return true;
    }
    if (code != STILL_ACTIVE) {
        state.last_exit_code = static_cast<int>(code);
        state.has_exit_code = true;
    }
    CloseHandle(state.process);
    state.process = nullptr;
    state.pid = 0;
    if (state.job) {
        CloseHandle(state.job);
        state.job = nullptr;
    }
#else
    if (state.pid <= 0) {
        return false;
    }
    int status = 0;
    const pid_t result = waitpid(state.pid, &status, WNOHANG);
    if (result == 0) {
        return true;
    }
    if (result == state.pid) {
        state.last_exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
        state.has_exit_code = true;
    }
    state.pid = -1;
#endif
    join_log_thread_locked(state);
    return false;
}

void begin_log_session_locked(app_state & state) {
    const std::string stem = "session-" + timestamp_string(true);
    state.log_session = stem + ".log";
    for (int suffix = 1; std::filesystem::exists(state.logs_directory / state.log_session); ++suffix) {
        state.log_session = stem + "-" + std::to_string(suffix) + ".log";
    }
    state.log_path = state.logs_directory / state.log_session;
    std::ofstream output(state.log_path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot create the SparkMoE session log");
    }
    output << '[' << timestamp_string(false) << "] SparkMoE " << SPARKMOE_VERSION << " session started\n";
}

void start_router_locked(app_state & state) {
    if (process_running_locked(state)) {
        throw std::runtime_error("the llama.cpp router is already running");
    }
    validate_router_config(state.router);
    validate_profile(state.default_profile);
    write_preset_locked(state);
    begin_log_session_locked(state);
    state.last_error.clear();
    state.has_exit_code = false;
    const std::vector<std::string> arguments = router_arguments(state);

#if defined(_WIN32)
    const std::filesystem::path executable = executable_directory() / L"llama-server.exe";
    if (!std::filesystem::is_regular_file(executable)) {
        throw std::runtime_error("llama-server.exe is not next to SparkMoE.exe");
    }
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    if (!CreatePipe(&read_pipe, &write_pipe, &security, 0) ||
            !SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0)) {
        if (read_pipe) CloseHandle(read_pipe);
        if (write_pipe) CloseHandle(write_pipe);
        throw std::runtime_error("cannot create the server log pipe");
    }
    HANDLE null_input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    std::wstring command = quote_windows_arg(executable.wstring());
    for (const auto & argument : arguments) {
        command += L" " + quote_windows_arg(utf8_to_wide(argument));
    }
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = null_input;
    startup.hStdOutput = write_pipe;
    startup.hStdError = write_pipe;
    PROCESS_INFORMATION process{};
    const DWORD flags = CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW | CREATE_SUSPENDED;
    if (!CreateProcessW(executable.c_str(), mutable_command.data(), nullptr, nullptr, TRUE,
            flags, nullptr, executable_directory().c_str(), &startup, &process)) {
        const DWORD error = GetLastError();
        CloseHandle(read_pipe);
        CloseHandle(write_pipe);
        if (null_input != INVALID_HANDLE_VALUE) CloseHandle(null_input);
        throw std::runtime_error("CreateProcessW failed with code " + std::to_string(error));
    }
    CloseHandle(write_pipe);
    if (null_input != INVALID_HANDLE_VALUE) CloseHandle(null_input);

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
                !AssignProcessToJobObject(job, process.hProcess)) {
            CloseHandle(job);
            job = nullptr;
        }
    }
    ResumeThread(process.hThread);
    CloseHandle(process.hThread);
    state.process = process.hProcess;
    state.job = job;
    state.pid = process.dwProcessId;
    const std::filesystem::path log_path = state.log_path;
    state.log_thread = std::thread([read_pipe, log_path]() {
        std::ofstream output(log_path, std::ios::binary | std::ios::app);
        std::vector<char> buffer(64 * 1024);
        DWORD bytes_read = 0;
        while (ReadFile(read_pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes_read, nullptr) && bytes_read > 0) {
            if (output) {
                output.write(buffer.data(), bytes_read);
                output.flush();
            }
        }
        CloseHandle(read_pipe);
    });
#else
    const std::filesystem::path executable = executable_directory() / "llama-server";
    int pipe_fds[2] = {-1, -1};
    if (pipe(pipe_fds) != 0) {
        throw std::runtime_error("cannot create the server log pipe");
    }
    const pid_t child = fork();
    if (child < 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        throw std::runtime_error("fork failed");
    }
    if (child == 0) {
        close(pipe_fds[0]);
        dup2(pipe_fds[1], STDOUT_FILENO);
        dup2(pipe_fds[1], STDERR_FILENO);
        close(pipe_fds[1]);
        std::vector<std::string> owned;
        owned.push_back(utf8_path(executable));
        owned.insert(owned.end(), arguments.begin(), arguments.end());
        std::vector<char *> argv;
        for (auto & argument : owned) {
            argv.push_back(argument.data());
        }
        argv.push_back(nullptr);
        execv(argv[0], argv.data());
        _exit(127);
    }
    close(pipe_fds[1]);
    state.pid = child;
    const std::filesystem::path log_path = state.log_path;
    state.log_thread = std::thread([read_fd = pipe_fds[0], log_path]() {
        std::ofstream output(log_path, std::ios::binary | std::ios::app);
        std::vector<char> buffer(64 * 1024);
        ssize_t count = 0;
        while ((count = read(read_fd, buffer.data(), buffer.size())) > 0) {
            if (output) {
                output.write(buffer.data(), count);
                output.flush();
            }
        }
        close(read_fd);
    });
#endif
    state.started_at_ms = now_ms();
}

void stop_router_locked(app_state & state) {
    if (!process_running_locked(state)) {
        return;
    }
#if defined(_WIN32)
    if (state.job) {
        TerminateJobObject(state.job, 0);
    } else {
        TerminateProcess(state.process, 0);
    }
    WaitForSingleObject(state.process, 5000);
    DWORD code = 0;
    if (GetExitCodeProcess(state.process, &code)) {
        state.last_exit_code = static_cast<int>(code);
        state.has_exit_code = true;
    }
    CloseHandle(state.process);
    state.process = nullptr;
    state.pid = 0;
    if (state.job) {
        CloseHandle(state.job);
        state.job = nullptr;
    }
#else
    kill(state.pid, SIGTERM);
    int status = 0;
    waitpid(state.pid, &status, 0);
    state.last_exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
    state.has_exit_code = true;
    state.pid = -1;
#endif
    join_log_thread_locked(state);
    state.started_at_ms = 0;
}

bool router_ready_locked(const app_state & state) {
    httplib::Client client("127.0.0.1", state.router.port);
    client.set_connection_timeout(0, 250000);
    client.set_read_timeout(0, 500000);
    const auto response = client.Get("/health");
    return response && response->status == 200;
}

json status_json(app_state & state) {
    std::lock_guard<std::mutex> lock(state.mutex);
    const bool running = process_running_locked(state);
    const bool ready = running && router_ready_locked(state);
    const std::string phase = ready ? "ready" : running ? "starting" : state.has_exit_code ? "exited" : "stopped";
    return {
        {"running", running},
        {"server_ready", ready},
        {"phase", phase},
        {"port", state.router.port},
        {"manager_port", manager_port()},
        {"started_at_ms", state.started_at_ms},
        {"last_error", state.last_error},
        {"last_exit_code", state.has_exit_code ? json(state.last_exit_code) : json(nullptr)},
        {"log_session", state.log_session},
        {"config", router_config_json(state.router)},
#if defined(_WIN32)
        {"pid", state.pid},
#else
        {"pid", state.pid > 0 ? state.pid : 0},
#endif
    };
}

json browse_models_directory() {
#if defined(_WIN32)
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool uninitialize = SUCCEEDED(initialized);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) {
        throw std::runtime_error("cannot initialize the Windows folder picker");
    }
    IFileOpenDialog * dialog = nullptr;
    HRESULT result = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    DWORD options = 0;
    if (SUCCEEDED(result)) result = dialog->GetOptions(&options);
    if (SUCCEEDED(result)) result = dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM |
        FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR | FOS_DONTADDTORECENT);
    if (SUCCEEDED(result)) result = dialog->SetTitle(L"Choose the folder that contains your GGUF models");
    if (SUCCEEDED(result)) result = dialog->Show(nullptr);
    if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        if (dialog) dialog->Release();
        if (uninitialize) CoUninitialize();
        return {{"cancelled", true}};
    }
    IShellItem * item = nullptr;
    if (SUCCEEDED(result)) result = dialog->GetResult(&item);
    PWSTR path = nullptr;
    if (SUCCEEDED(result)) result = item->GetDisplayName(SIGDN_FILESYSPATH, &path);
    const std::wstring selected = SUCCEEDED(result) && path ? path : L"";
    if (path) CoTaskMemFree(path);
    if (item) item->Release();
    if (dialog) dialog->Release();
    if (uninitialize) CoUninitialize();
    if (FAILED(result) || selected.empty()) {
        throw std::runtime_error("the selected folder does not have a local filesystem path");
    }
    return {{"cancelled", false}, {"directory", wide_to_utf8(selected)}};
#else
    throw std::runtime_error("native folder selection is currently available on Windows only");
#endif
}

std::filesystem::path resolve_log_session(const app_state & state, const std::string & session) {
    if (session.empty() || session.find('/') != std::string::npos || session.find('\\') != std::string::npos ||
            session.rfind("session-", 0) != 0 || session.size() < 13 ||
            session.substr(session.size() - 4) != ".log") {
        throw std::runtime_error("invalid log session");
    }
    const std::filesystem::path path = state.logs_directory / native_path(session);
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error) {
        throw std::runtime_error("log session does not exist");
    }
    return path;
}

json log_sessions_json(app_state & state) {
    std::lock_guard<std::mutex> lock(state.mutex);
    std::vector<json> sessions;
    std::error_code error;
    for (std::filesystem::directory_iterator it(state.logs_directory, error), end; !error && it != end; it.increment(error)) {
        if (!it->is_regular_file(error) || error) {
            error.clear();
            continue;
        }
        const std::string name = utf8_path(it->path().filename());
        if (name.rfind("session-", 0) != 0 || lowercase(utf8_path(it->path().extension())) != ".log") {
            continue;
        }
        const uintmax_t size = it->file_size(error);
        sessions.push_back({
            {"name", name},
            {"size", error ? 0 : size},
            {"current", name == state.log_session},
        });
        error.clear();
    }
    std::sort(sessions.begin(), sessions.end(), [](const json & left, const json & right) {
        return left["name"].get<std::string>() > right["name"].get<std::string>();
    });
    if (sessions.size() > 100) {
        sessions.resize(100);
    }
    return {{"sessions", sessions}, {"directory", utf8_path(state.logs_directory)}};
}

json read_log_json(app_state & state, const std::string & session, uint64_t requested_offset, bool has_offset, size_t limit) {
    std::filesystem::path path;
    std::string resolved_session;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        resolved_session = session.empty() ? state.log_session : session;
        path = resolve_log_session(state, resolved_session);
    }
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        throw std::runtime_error("cannot read the log session");
    }
    const uint64_t total = static_cast<uint64_t>(input.tellg());
    const uint64_t offset = has_offset ? std::min(requested_offset, total) : (total > limit ? total - limit : 0);
    const size_t count = static_cast<size_t>(std::min<uint64_t>(limit, total - offset));
    std::string content(count, '\0');
    input.seekg(static_cast<std::streamoff>(offset));
    input.read(content.data(), static_cast<std::streamsize>(count));
    content.resize(static_cast<size_t>(input.gcount()));
    return {
        {"session", resolved_session},
        {"content", content},
        {"offset", offset},
        {"next_offset", offset + content.size()},
        {"total_size", total},
        {"truncated", offset > 0},
    };
}

void open_browser(int port) {
    if (std::getenv("SPARKMOE_NO_BROWSER")) {
        return;
    }
    const std::string url = "http://127.0.0.1:" + std::to_string(port) + "/";
#if defined(_WIN32)
    ShellExecuteW(nullptr, L"open", utf8_to_wide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    const pid_t child = fork();
    if (child == 0) {
        execlp("xdg-open", "xdg-open", url.c_str(), static_cast<char *>(nullptr));
        _exit(127);
    }
#endif
}

void open_browser_when_ready(app_state & state) {
    int port = 8080;
    for (int attempt = 0; attempt < 120; ++attempt) {
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            port = state.router.port;
            if (!process_running_locked(state)) {
                break;
            }
            if (router_ready_locked(state)) {
                open_browser(port);
                return;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    open_browser(port);
}

std::string request_origin(const httplib::Request & request) {
    return request.has_header("Origin") ? request.get_header_value("Origin") : std::string();
}

bool origin_allowed(const httplib::Request & request, app_state & state) {
    const std::string origin = request_origin(request);
    if (origin.empty()) {
        return true;
    }
    int port = 8080;
    int previous_port = 0;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        port = state.router.port;
        previous_port = state.previous_router_port;
    }
    std::vector<std::string> allowed = {
        "http://127.0.0.1:" + std::to_string(manager_port()),
        "http://localhost:" + std::to_string(manager_port()),
        "http://127.0.0.1:" + std::to_string(port),
        "http://localhost:" + std::to_string(port),
    };
    if (previous_port > 0) {
        allowed.push_back("http://127.0.0.1:" + std::to_string(previous_port));
        allowed.push_back("http://localhost:" + std::to_string(previous_port));
    }
    return std::find(allowed.begin(), allowed.end(), origin) != allowed.end();
}

void set_cors_headers(const httplib::Request & request, httplib::Response & response) {
    const std::string origin = request_origin(request);
    if (!origin.empty()) {
        response.set_header("Access-Control-Allow-Origin", origin);
        response.set_header("Vary", "Origin");
    }
    response.set_header("Access-Control-Allow-Headers", "Content-Type");
    response.set_header("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
}

void json_response(httplib::Response & response, const json & value, int status = 200) {
    response.status = status;
    response.set_content(value.dump(-1, ' ', false, json::error_handler_t::replace), "application/json; charset=utf-8");
}

bool reload_router_models(const router_config & config) {
    httplib::Client client("127.0.0.1", config.port);
    client.set_connection_timeout(0, 500000);
    client.set_read_timeout(60, 0);
    const auto response = client.Get("/models?reload=1");
    return response && response->status >= 200 && response->status < 300;
}

int run_manager() {
    app_state state;
    if (const char * port = std::getenv("SPARKMOE_SERVER_PORT")) {
        state.router.port = std::stoi(port);
    }
    state.data_directory = local_data_directory();
#if defined(_WIN32)
    _wputenv_s(L"SPARKMOE_DATA_DIR", L"");
#else
    unsetenv("SPARKMOE_DATA_DIR");
#endif
    state.config_path = state.data_directory / "config.json";
    state.preset_path = state.data_directory / "model-profiles.ini";
    state.logs_directory = state.data_directory / "logs";
    std::filesystem::create_directories(state.logs_directory);
    configure_initial_recommendation(state);
    try {
        load_config(state);
    } catch (const std::exception & error) {
        std::lock_guard<std::mutex> lock(state.mutex);
        begin_log_session_locked(state);
        state.last_error = std::string("Saved configuration was not loaded: ") + error.what();
        append_log(state, state.last_error);
        save_config_locked(state);
    }
    try {
        std::lock_guard<std::mutex> lock(state.mutex);
        start_router_locked(state);
    } catch (const std::exception & error) {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.last_error = error.what();
        append_log(state, std::string("Router start failed: ") + error.what());
    }

    httplib::Server server;
    auto authorize = [&](const httplib::Request & request, httplib::Response & response) {
        if (!origin_allowed(request, state)) {
            json_response(response, {{"error", "request origin is not allowed"}}, 403);
            return false;
        }
        set_cors_headers(request, response);
        return true;
    };

    server.Get("/", [&](const httplib::Request &, httplib::Response & response) {
        int port = 8080;
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            port = state.router.port;
        }
        response.status = 302;
        response.set_header("Location", "http://127.0.0.1:" + std::to_string(port) + "/#/sparkmoe");
    });
    server.Options(R"(/api/.*)", [&](const httplib::Request & request, httplib::Response & response) {
        if (authorize(request, response)) response.status = 204;
    });
    server.Get("/api/status", [&](const httplib::Request & request, httplib::Response & response) {
        if (!authorize(request, response)) return;
        json_response(response, status_json(state));
    });
    server.Get("/api/config", [&](const httplib::Request & request, httplib::Response & response) {
        if (!authorize(request, response)) return;
        std::lock_guard<std::mutex> lock(state.mutex);
        json_response(response, router_config_json(state.router));
    });
    server.Post("/api/config", [&](const httplib::Request & request, httplib::Response & response) {
        if (!authorize(request, response)) return;
        try {
            const json input = json::parse(request.body);
            router_config next;
            {
                std::lock_guard<std::mutex> lock(state.mutex);
                next = router_config_from_json(input, state.router);
            }
            validate_router_config(next);
            {
                std::lock_guard<std::mutex> lock(state.mutex);
                stop_router_locked(state);
                state.previous_router_port = state.router.port;
                state.router = next;
                save_config_locked(state);
                start_router_locked(state);
            }
            append_log(state, "Router configuration saved");
            json_response(response, {{"ok", true}, {"port", next.port}, {"status", status_json(state)}});
        } catch (const std::exception & error) {
            {
                std::lock_guard<std::mutex> lock(state.mutex);
                state.last_error = error.what();
            }
            append_log(state, std::string("Configuration update failed: ") + error.what());
            json_response(response, {{"error", error.what()}}, 400);
        }
    });
    server.Get("/api/recommendation", [&](const httplib::Request & request, httplib::Response & response) {
        if (!authorize(request, response)) return;
        json_response(response, recommendation_json());
    });
    server.Post("/api/browse-model-directory", [&](const httplib::Request & request, httplib::Response & response) {
        if (!authorize(request, response)) return;
        try {
            json_response(response, browse_models_directory());
        } catch (const std::exception & error) {
            json_response(response, {{"error", error.what()}}, 500);
        }
    });
    server.Get("/api/model-profile", [&](const httplib::Request & request, httplib::Response & response) {
        if (!authorize(request, response)) return;
        try {
            const std::string model = request.has_param("model") ? request.get_param_value("model") : "";
            validate_model_id(model);
            std::lock_guard<std::mutex> lock(state.mutex);
            const auto found = state.profiles.find(model);
            json_response(response, {
                {"model", model},
                {"is_override", found != state.profiles.end()},
                {"profile", profile_json(found == state.profiles.end() ? state.default_profile : found->second)},
            });
        } catch (const std::exception & error) {
            json_response(response, {{"error", error.what()}}, 400);
        }
    });
    server.Post("/api/model-profile", [&](const httplib::Request & request, httplib::Response & response) {
        if (!authorize(request, response)) return;
        try {
            const json input = json::parse(request.body);
            const std::string model = input.value("model", std::string());
            validate_model_id(model);
            router_config router;
            {
                std::lock_guard<std::mutex> lock(state.mutex);
                const auto found = state.profiles.find(model);
                const model_profile base = found == state.profiles.end() ? state.default_profile : found->second;
                model_profile profile = profile_from_json(input.value("profile", json::object()), base);
                validate_profile(profile);
                state.profiles[model] = profile;
                save_config_locked(state);
                router = state.router;
            }
            const bool reloaded = reload_router_models(router);
            append_log(state, "Saved model profile for " + model);
            json_response(response, {{"ok", true}, {"model", model}, {"router_reloaded", reloaded}});
        } catch (const std::exception & error) {
            append_log(state, std::string("Model profile update failed: ") + error.what());
            json_response(response, {{"error", error.what()}}, 400);
        }
    });
    server.Delete("/api/model-profile", [&](const httplib::Request & request, httplib::Response & response) {
        if (!authorize(request, response)) return;
        try {
            const std::string model = request.has_param("model") ? request.get_param_value("model") : "";
            validate_model_id(model);
            router_config router;
            {
                std::lock_guard<std::mutex> lock(state.mutex);
                state.profiles.erase(model);
                save_config_locked(state);
                router = state.router;
            }
            const bool reloaded = reload_router_models(router);
            append_log(state, "Reset model profile for " + model);
            json_response(response, {{"ok", true}, {"router_reloaded", reloaded}});
        } catch (const std::exception & error) {
            json_response(response, {{"error", error.what()}}, 400);
        }
    });
    server.Post("/api/start", [&](const httplib::Request & request, httplib::Response & response) {
        if (!authorize(request, response)) return;
        try {
            std::lock_guard<std::mutex> lock(state.mutex);
            start_router_locked(state);
            json_response(response, {{"ok", true}});
        } catch (const std::exception & error) {
            json_response(response, {{"error", error.what()}}, 400);
        }
    });
    server.Post("/api/restart", [&](const httplib::Request & request, httplib::Response & response) {
        if (!authorize(request, response)) return;
        try {
            std::lock_guard<std::mutex> lock(state.mutex);
            stop_router_locked(state);
            start_router_locked(state);
            json_response(response, {{"ok", true}});
        } catch (const std::exception & error) {
            json_response(response, {{"error", error.what()}}, 400);
        }
    });
    server.Post("/api/stop", [&](const httplib::Request & request, httplib::Response & response) {
        if (!authorize(request, response)) return;
        std::lock_guard<std::mutex> lock(state.mutex);
        stop_router_locked(state);
        json_response(response, {{"ok", true}});
    });
    server.Post("/api/open", [&](const httplib::Request & request, httplib::Response & response) {
        if (!authorize(request, response)) return;
        int port = 8080;
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            port = state.router.port;
        }
        open_browser(port);
        json_response(response, {{"ok", true}});
    });
    server.Get("/api/log-sessions", [&](const httplib::Request & request, httplib::Response & response) {
        if (!authorize(request, response)) return;
        json_response(response, log_sessions_json(state));
    });
    server.Get("/api/logs", [&](const httplib::Request & request, httplib::Response & response) {
        if (!authorize(request, response)) return;
        try {
            const std::string session = request.has_param("session") ? request.get_param_value("session") : "";
            const bool has_offset = request.has_param("offset");
            const uint64_t offset = has_offset ? std::stoull(request.get_param_value("offset")) : 0;
            const size_t requested_limit = request.has_param("limit") ?
                static_cast<size_t>(std::stoull(request.get_param_value("limit"))) : 131072;
            const size_t limit = std::max<size_t>(1, std::min(requested_limit, max_log_read_bytes));
            json_response(response, read_log_json(state, session, offset, has_offset, limit));
        } catch (const std::exception & error) {
            json_response(response, {{"error", error.what()}}, 400);
        }
    });
    server.Get("/api/logs/download", [&](const httplib::Request & request, httplib::Response & response) {
        if (!authorize(request, response)) return;
        try {
            std::filesystem::path path;
            std::string session;
            {
                std::lock_guard<std::mutex> lock(state.mutex);
                session = request.has_param("session") ? request.get_param_value("session") : state.log_session;
                path = resolve_log_session(state, session);
            }
            std::ifstream input(path, std::ios::binary);
            std::ostringstream content;
            content << input.rdbuf();
            response.set_header("Content-Disposition", "attachment; filename=\"" + session + "\"");
            response.set_content(content.str(), "text/plain; charset=utf-8");
        } catch (const std::exception & error) {
            json_response(response, {{"error", error.what()}}, 400);
        }
    });
    server.Get("/api/diagnostics", [&](const httplib::Request & request, httplib::Response & response) {
        if (!authorize(request, response)) return;
        json diagnostic;
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            diagnostic = {
                {"sparkmoe_version", SPARKMOE_VERSION},
                {"router", router_config_json(state.router)},
                {"profile_count", state.profiles.size()},
                {"log_session", state.log_session},
#if defined(_WIN32)
                {"platform", "windows"},
#else
                {"platform", "posix"},
#endif
                {"privacy", "No prompts, generated text, API keys, tokens, usernames, or model contents are collected."},
            };
            diagnostic["router"].erase("model_directory");
        }
        response.set_header("Content-Disposition", "attachment; filename=sparkmoe-diagnostics.json");
        json_response(response, diagnostic);
    });
    server.set_exception_handler([](const auto &, auto & response, std::exception_ptr error) {
        try {
            if (error) std::rethrow_exception(error);
        } catch (const std::exception & exception) {
            json_response(response, {{"error", exception.what()}}, 500);
        }
    });

    std::thread browser_thread([&state]() { open_browser_when_ready(state); });
    const bool ok = server.listen("127.0.0.1", manager_port());
    if (browser_thread.joinable()) browser_thread.join();
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        stop_router_locked(state);
    }
    return ok ? 0 : 1;
}

} // namespace

#if defined(_WIN32)
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    std::wstring mutex_name = L"Local\\SparkMoEManager";
    if (manager_port() != default_manager_port) {
        mutex_name += L"-" + std::to_wstring(manager_port());
    }
    HANDLE instance = CreateMutexW(nullptr, TRUE, mutex_name.c_str());
    if (!instance) {
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(instance);
        httplib::Client client("127.0.0.1", manager_port());
        client.set_connection_timeout(0, 500000);
        const auto response = client.Get("/api/status");
        int port = 8080;
        if (response && response->status == 200) {
            try {
                port = json::parse(response->body).value("port", port);
            } catch (...) {
            }
        }
        open_browser(port);
        return 0;
    }
    const int result = run_manager();
    ReleaseMutex(instance);
    CloseHandle(instance);
    return result;
}
#else
int main() {
    return run_manager();
}
#endif
