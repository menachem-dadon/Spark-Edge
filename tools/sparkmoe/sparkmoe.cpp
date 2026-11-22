// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright (c) 2026 SparkMoE contributors.

#include "httplib.h"
#include "model-library.h"
#include "http-facade.h"
#include "desktop-window.h"
#include "connections.h"
#include "downloads.h"
#include "engine-packages.h"
#include "ui.h"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <condition_variable>
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
    std::string context_mode = "auto";
    std::string memory_mode = "auto";
    int gpu_layers = 0;
    int gpu_expert_cache_mib = 0;
    std::string worker_engine;
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
    std::string reasoning = "default";
    std::string speculative_types;
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
    router_config applied_router;
    model_profile default_profile;
    std::map<std::string, json> overrides;
    std::map<std::string, json> applied_recipes;
    std::map<std::string, model_profile> applied_profiles;
    std::map<std::string, uint64_t> profile_revisions;
    json library = json::array();
    json application = {{"background_mode", false}, {"launch_at_login", false}, {"default_model", ""}, {"last_loaded_model", ""}, {"locale", "he"}};
    std::unordered_set<std::string> loaded_models;
    json last_model_status = json::array();
    std::string engine_id;
    std::mutex engine_download_mutex;
    std::unordered_set<std::string> removed_paths;
    uint64_t revision = 1;
    std::mutex operation_mutex;
    std::atomic<int> active_requests{0};
    std::atomic<bool> background_mode{false};
    std::atomic<bool> hebrew{true};
    std::function<void()> open_ui;
    std::function<void *()> native_window;
    std::filesystem::path data_directory;
    std::filesystem::path config_path;
    std::filesystem::path preset_path;
    std::filesystem::path logs_directory;
    std::filesystem::path log_path;
    std::string log_session;
    std::string last_error;
    std::string configuration_warning;
    bool configuration_read_only = false;
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
        {"context_mode", profile.context_mode},
        {"memory_mode", profile.memory_mode},
        {"gpu_layers", profile.gpu_layers},
        {"gpu_expert_cache_mib", profile.gpu_expert_cache_mib},
        {"worker_engine", profile.worker_engine},
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
        {"speculative_types", profile.speculative_types},
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
    profile.context_mode = input.value("context_mode", profile.context_mode);
    profile.memory_mode = input.value("memory_mode", profile.memory_mode);
    profile.gpu_layers = input.value("gpu_layers", profile.gpu_layers);
    profile.gpu_expert_cache_mib = input.value("gpu_expert_cache_mib", profile.gpu_expert_cache_mib);
    profile.worker_engine = input.value("worker_engine", profile.worker_engine);
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
    if (profile.reasoning == "auto") profile.reasoning = "default";
    if (profile.reasoning == "on") profile.reasoning = "max";
    profile.speculative_types = input.value("speculative_types", profile.speculative_types);
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

std::string speculative_types(const model_profile & profile) {
    return profile.speculative_types.empty() ? (profile.mtp_enabled ? "draft-mtp" : "none") : profile.speculative_types;
}

void validate_profile(const model_profile & profile, bool validate_files = true) {
    static const std::unordered_set<std::string> supported = {
        "none", "draft-simple", "draft-eagle3", "draft-mtp", "draft-dflash", "draft-dflash2", "draft-dspark", "draft-lilicorr",
        "ngram-simple", "ngram-map-k", "ngram-map-k4v", "ngram-mod", "ngram-cache",
    };
    if (profile.gpu_expert_cache_mib < 0 || profile.gpu_expert_cache_mib > 262144) {
        throw std::runtime_error("Choose a GPU expert cache between 0 and 262144 MiB.");
    }
    const auto modes = speculative_types(profile);
    std::istringstream methods(modes);
    std::unordered_set<std::string> used;
    std::string method;
    int drafts = 0;
    while (std::getline(methods, method, ',')) {
        if (!supported.count(method) || !used.insert(method).second) {
            throw std::runtime_error("Choose supported speculative decoding methods without duplicates.");
        }
        if (method.rfind("draft-", 0) == 0) ++drafts;
    }
    if (modes.back() == ',' || drafts > 1 || (used.count("none") && used.size() != 1)) {
        throw std::runtime_error("Choose one draft method and compatible n-gram methods, or turn speculation off.");
    }
    if (drafts && !used.count("draft-mtp") && profile.mtp_model.empty()) {
        throw std::runtime_error("This speculative decoding method requires a compatible draft GGUF.");
    }
    static const std::unordered_set<std::string> cache_types = {
        "f16", "bf16", "q8_0", "q4_0", "q4_1", "iq4_nl", "q5_0", "q5_1",
    };
    if ((profile.context_mode != "auto" && profile.context_mode != "manual") ||
            (profile.memory_mode != "auto" && profile.memory_mode != "manual") || profile.gpu_layers < -1 || profile.gpu_layers > 4096) {
        throw std::runtime_error("invalid automatic model settings");
    }
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
    if (profile.reasoning != "default" && profile.reasoning != "off" && profile.reasoning != "low" &&
            profile.reasoning != "medium" && profile.reasoning != "high" && profile.reasoning != "max") {
        throw std::runtime_error("invalid reasoning effort");
    }
    if (validate_files && !local_gguf_is_valid(profile.mtp_model)) {
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
    output << "n-gpu-layers = " << profile.gpu_layers << '\n';
    if (profile.gpu_expert_cache_mib > 0) output << "moe-cache-mib = " << profile.gpu_expert_cache_mib << '\n';
    output << "ctx-size = " << profile.ctx_size << '\n';
    output << "batch-size = " << profile.batch_size << '\n';
    output << "ubatch-size = " << profile.ubatch_size << '\n';
    output << "parallel = " << profile.parallel << '\n';
    output << "ctx-checkpoints = " << profile.ctx_checkpoints << '\n';
    output << "cache-ram = " << profile.cache_ram_mib << '\n';
    output << "cache-type-k = " << profile.cache_type_k << '\n';
    output << "cache-type-v = " << profile.cache_type_v << '\n';
    output << "context-shift = " << (profile.context_shift ? "true" : "false") << '\n';
    if (profile.reasoning == "default") {
        output << "reasoning = auto\n";
    } else if (profile.reasoning == "off") {
        output << "reasoning = off\n";
    } else {
        output << "reasoning = on\n";
        output << "reasoning-effort = " << profile.reasoning << '\n';
        if (profile.reasoning == "low") output << "reasoning-budget = 512\n";
        if (profile.reasoning == "medium") output << "reasoning-budget = 2048\n";
        if (profile.reasoning == "high") output << "reasoning-budget = 8192\n";
        if (profile.reasoning == "max") output << "reasoning-budget = -1\n";
    }
    output << "moe-paging = " << profile.mode << '\n';
    if (profile.cache > 0) {
        output << "moe-paging-cache-mib = " << profile.cache << '\n';
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
    const auto speculation = speculative_types(profile);
    output << "spec-type = " << speculation << '\n';
    if (speculation != "none") {
        if (speculation.find("draft-") != std::string::npos && !profile.mtp_model.empty()) {
            output << "model-draft = " << profile.mtp_model << '\n';
        }
        if (speculation.find("draft-mtp") != std::string::npos) {
            output << "moe-draft-cache-mib = " << profile.mtp_cache_mib << '\n';
        }
        output << "spec-draft-n-max = " << profile.mtp_draft_tokens << '\n';
        output << "spec-draft-n-min = " << profile.mtp_min_tokens << '\n';
        output << "spec-draft-p-min = " << profile.mtp_p_min << '\n';
        output << "spec-draft-backend-sampling = " << (profile.mtp_backend_sampling ? "true" : "false") << '\n';
        if (profile.mtp_threads > 0) {
            output << "spec-draft-threads = " << profile.mtp_threads << '\n';
        }
    }
}

model_profile resolved_profile(const app_state & state, const std::string & model);

json recipe_locked(const app_state & state, const std::string & model) {
    json recipe = profile_json(state.default_profile);
    const auto found = state.overrides.find(model);
    if (found != state.overrides.end()) recipe.update(found->second);
    return recipe;
}

std::string canonical_model(const app_state & state, const std::string & id) {
    for (const auto & model : state.library) if (model["id"] == id) return id;
    for (const auto & model : state.library) {
        for (const auto & alias : model.value("legacy_ids", json::array())) {
            if (alias == id) return model["id"].get<std::string>();
        }
    }
    return id;
}

void write_preset_locked(app_state & state) {
    std::ostringstream output;
    output << "# Generated by SparkMoE. Edit model settings in the app.\n\n[*]\n";
    write_profile(output, state.default_profile);
#if defined(_WIN32)
    const sparkmoe::engine_packages engines(state.data_directory / "engines");
    std::map<std::string, std::string> workers;
#endif
    for (const auto & model : state.library) {
        if (!model.value("available", false)) continue;
        const auto id = model["id"].get<std::string>();
        validate_model_id(id);
        const auto path = model["path"].get<std::string>();
        validate_ini_value(path);
        output << "\n[" << id << "]\nmodel = " << path << '\n';
        std::string aliases;
        for (const auto & alias : model.value("legacy_ids", json::array())) {
            const auto value = alias.get<std::string>();
            validate_ini_value(value);
            if (value != id && value.find(',') == std::string::npos && canonical_model(state, value) == id) {
                if (!aliases.empty()) aliases += ',';
                aliases += value;
            }
        }
        if (!aliases.empty()) output << "alias = " << aliases << '\n';
        const auto projector = model.value("mmproj", std::string());
        if (!projector.empty()) { validate_ini_value(projector); output << "mmproj = " << projector << '\n'; }
        const auto applied = state.applied_profiles.find(id);
        const auto profile = applied == state.applied_profiles.end() ? resolved_profile(state, id) : applied->second;
        write_profile(output, profile);
#if defined(_WIN32)
        if (!profile.worker_engine.empty()) {
            auto [entry, first] = workers.try_emplace(profile.worker_engine);
            if (first) {
                try { entry->second = utf8_path(engines.executable(profile.worker_engine)); }
                catch (const std::exception & error) {
                    append_log(state, std::string("Using the default engine: ") + error.what());
                }
            }
            if (!entry->second.empty()) {
                validate_ini_value(entry->second);
                output << "spark-edge-engine = " << entry->second << '\n';
            }
        }
#endif
    }
    sparkmoe::atomic_write(state.preset_path, output.str(), false);
}

void save_config_locked(app_state & state) {
    if (state.configuration_read_only) throw std::runtime_error("These settings require a newer SparkMoE version. Install that version before saving changes.");
    const json root = {
        {"version", 2},
        {"revision", state.revision},
        {"router", router_config_json(state.router)},
        {"default_profile", profile_json(state.default_profile)},
        {"profiles", state.overrides},
        {"profile_revisions", state.profile_revisions},
        {"library", state.library},
        {"removed_paths", state.removed_paths},
        {"application", state.application},
    };
    sparkmoe::atomic_write(state.config_path, root.dump(2) + '\n');
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
    const int version = root.value("version", 1);
    if (version > 2) throw std::runtime_error("Settings were created by a newer SparkMoE version.");
    router_config router = router_config_from_json(root.value("router", json::object()), state.router);
    model_profile default_profile = profile_from_json(
        root.value("default_profile", json::object()), state.default_profile);
    if (version == 1) {
        default_profile.context_mode = "manual";
        default_profile.memory_mode = "manual";
    }
    std::map<std::string, json> profiles;
    const json saved_profiles = root.value("profiles", json::object());
    if (!saved_profiles.is_object()) {
        throw std::runtime_error("saved model profiles are invalid");
    }
    if (router.port < 1024 || router.port > 65535 || router.port == manager_port()) throw std::runtime_error("Invalid saved server port.");
    validate_profile(default_profile, false);
    for (const auto & item : saved_profiles.items()) {
        validate_model_id(item.key());
        model_profile profile = profile_from_json(item.value(), default_profile);
        validate_profile(profile, false);
        profiles[item.key()] = version == 1 ? profile_json(profile) : item.value();
    }
    std::lock_guard<std::mutex> lock(state.mutex);
    state.router = router;
    state.default_profile = default_profile;
    state.overrides = std::move(profiles);
    state.library = root.value("library", json::array());
    state.application.update(root.value("application", json::object()));
    state.background_mode = state.application.value("background_mode", false);
    state.hebrew = state.application.value("locale", "he") == std::string("he");
    state.revision = root.value("revision", uint64_t(1));
    state.profile_revisions = root.value("profile_revisions", std::map<std::string, uint64_t>());
    state.removed_paths = root.value("removed_paths", std::unordered_set<std::string>());
    if (version == 1) std::filesystem::copy_file(state.config_path, state.data_directory / "config-v1-backup.json", std::filesystem::copy_options::skip_existing);
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

json model_recommendation(const app_state & state, const std::string & id) {
    json result = recommendation_json();
    json metadata = json::object();
    for (const auto & model : state.library) if (model["id"] == id) metadata = model.value("metadata", json::object());
    auto number = [&](const char * key, uint64_t fallback = 0) -> uint64_t {
        return metadata.contains(key) && metadata[key].is_number_integer() && metadata[key].get<int64_t>() > 0 ? metadata[key].get<uint64_t>() : fallback;
    };
    const uint64_t total = result["total_ram_mib"].get<uint64_t>();
    const uint64_t available = result["available_ram_mib"].get<uint64_t>();
    const uint64_t reserve = std::max<uint64_t>(1024, total / 8);
    const uint64_t budget = available > reserve ? available - reserve : 0;
    const uint64_t resident = number("resident_bytes") / 1048576;
    const uint64_t layers = number("block_count", 32);
    const uint64_t heads = number("attention.head_count", 32);
    const uint64_t kv_heads = number("attention.head_count_kv", heads);
    const uint64_t dimension = number("embedding_length", 4096) / std::max<uint64_t>(1, heads);
    const uint64_t kv_per_token = 2 * layers * kv_heads *
        (number("attention.key_length", dimension) + number("attention.value_length", dimension));
    uint64_t context = std::min<uint64_t>(8192, number("context_length", 4096));
    const uint64_t free = budget > resident + 512 ? budget - resident - 512 : 0;
    while (context > 512 && (context * kv_per_token / 1048576) > free / 2) context /= 2;
    const uint64_t kv = context * kv_per_token / 1048576;
    const uint64_t cache = std::max<uint64_t>(64, std::min<uint64_t>(8192, free > kv ? free - kv : 64));
    result["ctx_size"] = context;
    result["cache_mib"] = cache;
    result["estimated_kv_mib"] = kv;
    result["resident_mib"] = resident;
    result["headroom_mib"] = reserve;
    result["estimate"] = true;
    result["memory_fit"] = budget > resident + kv + 512 ? "estimated" : "limited";
    result["reason"] = budget > resident + kv + 512 ? "Based on model metadata and available memory." : "Available memory is limited. A smaller model or context may be needed.";
    result["supports_reasoning"] = metadata.value("supports_reasoning", json(nullptr));
    result["supports_mtp"] = metadata.value("supports_mtp", json(false));
    result["ubatch_size"] = std::min<uint64_t>(128, std::max<uint64_t>(2, free / 64));
    return result;
}

model_profile resolved_profile(const app_state & state, const std::string & id) {
    model_profile profile = profile_from_json(recipe_locked(state, id), state.default_profile);
    const json recommendation = model_recommendation(state, id);
    if (profile.context_mode == "auto") profile.ctx_size = recommendation["ctx_size"].get<int>();
    if (profile.memory_mode == "auto") profile.cache = recommendation["cache_mib"].get<int>();
    if (profile.context_mode == "auto" && profile.memory_mode == "auto" &&
            (!state.overrides.count(id) || !state.overrides.at(id).contains("ubatch_size"))) {
        profile.ubatch_size = std::min(profile.batch_size, recommendation["ubatch_size"].get<int>());
    }
    return profile;
}

json model_profile_response(app_state & state, const std::string & requested) {
    const std::string id = canonical_model(state, requested);
    const auto recipe = recipe_locked(state, id);
    const auto applied = state.applied_recipes.find(id);
    return {{"model", id}, {"is_override", state.overrides.count(id) != 0}, {"profile", recipe},
        {"revision", state.profile_revisions[id]}, {"recommendation", model_recommendation(state, id)},
        {"applied_profile", state.applied_profiles.count(id) ? profile_json(state.applied_profiles.at(id)) : json(nullptr)},
        {"pending", applied != state.applied_recipes.end() && applied->second != recipe}};
}

void merge_library_locked(app_state & state, const json & discovered, bool explicit_import = false) {
    const auto previous_library = state.library;
    const auto previous_overrides = state.overrides;
    const auto previous_removed = state.removed_paths;
    for (auto entry : discovered) {
        const auto path = entry["path"].get<std::string>();
        if (explicit_import) state.removed_paths.erase(path);
        if (state.removed_paths.count(path)) continue;
        auto existing = std::find_if(state.library.begin(), state.library.end(), [&](const json & model) { return model["path"] == path; });
        if (existing != state.library.end()) {
            entry["id"] = (*existing)["id"];
            entry["display_name"] = (*existing)["display_name"];
            entry["favorite"] = (*existing)["favorite"];
            for (const auto & alias : (*existing)["legacy_ids"]) {
                if (std::find(entry["legacy_ids"].begin(), entry["legacy_ids"].end(), alias) == entry["legacy_ids"].end()) entry["legacy_ids"].push_back(alias);
            }
            *existing = entry;
        } else {
            const auto stem = entry["id"].get<std::string>();
            std::string id = stem;
            for (int suffix = 2; std::any_of(state.library.begin(), state.library.end(), [&](const json & model) { return model["id"] == id; }); ++suffix) id = stem + "-" + std::to_string(suffix);
            entry["id"] = id;
            state.library.push_back(entry);
        }
        const auto id = entry["id"].get<std::string>();
        if (!state.overrides.count(id)) {
            for (const auto & alias : entry["legacy_ids"]) {
                const auto found = state.overrides.find(alias.get<std::string>());
                if (found != state.overrides.end()) {
                    state.overrides[id] = found->second;
                    if (found->first != id) state.overrides.erase(found);
                    break;
                }
            }
        }
    }
    ++state.revision;
    try { save_config_locked(state); }
    catch (...) {
        state.library = previous_library;
        state.overrides = previous_overrides;
        state.removed_paths = previous_removed;
        --state.revision;
        throw;
    }
}

json library_response_locked(app_state & state) {
    for (auto & model : state.library) {
        std::error_code error;
        bool present = std::filesystem::is_regular_file(native_path(model["path"].get<std::string>()), error);
        for (const auto & file : model.value("files", json::array())) {
            present = present && std::filesystem::is_regular_file(native_path(file.get<std::string>()), error);
        }
        model["available"] = present && !error && !model.contains("error");
        model["missing"] = !present || bool(error);
    }
    return {{"models", state.library}, {"revision", state.revision}};
}

void apply_all_locked(app_state & state) {
    state.applied_router = state.router;
    for (const auto & model : state.library) {
        const auto id = model["id"].get<std::string>();
        state.applied_profiles[id] = resolved_profile(state, id);
        state.applied_recipes[id] = recipe_locked(state, id);
    }
    write_preset_locked(state);
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
    apply_all_locked(state);
    begin_log_session_locked(state);
    state.last_error.clear();
    state.has_exit_code = false;
    const std::vector<std::string> arguments = router_arguments(state);

#if defined(_WIN32)
    const std::filesystem::path executable = state.engine_id.empty()
        ? executable_directory() / L"llama-server.exe"
        : sparkmoe::engine_packages(state.data_directory / "engines").executable(state.engine_id);
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
            flags, nullptr, executable.parent_path().c_str(), &startup, &process)) {
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
    httplib::Client client("127.0.0.1", state.applied_router.port);
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
        {"port", state.applied_router.port},
        {"revision", state.revision},
        {"pending", router_config_json(state.router) != router_config_json(state.applied_router)},
        {"active_requests", state.active_requests.load()},
        {"engine", state.engine_id},
        {"upstream_commit", SPARK_EDGE_UPSTREAM},
        {"manager_port", manager_port()},
        {"started_at_ms", state.started_at_ms},
        {"last_error", state.last_error},
        {"configuration_warning", state.configuration_warning},
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

json browse_models_directory(const std::string & initial_directory, bool file = false, void * owner_window = nullptr) {
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
    if (SUCCEEDED(result)) result = dialog->SetOptions(options | (file ? FOS_FILEMUSTEXIST : FOS_PICKFOLDERS) | FOS_FORCEFILESYSTEM |
        FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR | FOS_DONTADDTORECENT);
    if (SUCCEEDED(result)) result = dialog->SetTitle(file ? L"Add model" : L"Add model folder");
    if (SUCCEEDED(result)) result = dialog->SetOkButtonLabel(L"Select");
    const COMDLG_FILTERSPEC filters[] = {{L"GGUF models", L"*.gguf"}};
    if (SUCCEEDED(result) && file) result = dialog->SetFileTypes(1, filters);
    if (SUCCEEDED(result) && !initial_directory.empty()) {
        IShellItem * initial_item = nullptr;
        const HRESULT folder_result = SHCreateItemFromParsingName(
            utf8_to_wide(initial_directory).c_str(), nullptr, IID_PPV_ARGS(&initial_item));
        if (SUCCEEDED(folder_result)) {
            dialog->SetFolder(initial_item);
            initial_item->Release();
        }
    }
    HWND owner = static_cast<HWND>(owner_window);
    if (owner && !IsWindowVisible(owner)) owner = nullptr;
    if (SUCCEEDED(result)) result = dialog->Show(owner);
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
    return {{"cancelled", false}, {"directory", wide_to_utf8(selected)}, {"path", wide_to_utf8(selected)}};
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

void open_browser(int port, const std::string & route = "/") {
    if (std::getenv("SPARKMOE_NO_BROWSER")) {
        return;
    }
    const std::string url = "http://127.0.0.1:" + std::to_string(port) + route;
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

#if defined(_WIN32)
constexpr int sparkmoe_icon_resource = 101;
constexpr UINT tray_callback_message = WM_APP + 1;
constexpr UINT tray_timer_id = 1;
constexpr UINT tray_command_open = 1001;
constexpr UINT tray_command_start = 1002;
constexpr UINT tray_command_stop = 1003;
constexpr UINT tray_command_exit = 1004;

class notification_area {
public:
    notification_area(app_state & state, httplib::Server & server) : state_(state), server_(server) {
    }

    ~notification_area() {
        stop();
    }

    bool start() {
        thread_ = std::thread([this]() { run(); });
        std::unique_lock<std::mutex> lock(lifecycle_mutex_);
        lifecycle_condition_.wait(lock, [this]() { return initialized_; });
        const bool ready = ready_;
        lock.unlock();
        if (!ready && thread_.joinable()) {
            thread_.join();
        }
        return ready;
    }

    void stop() {
        HWND window = nullptr;
        {
            std::lock_guard<std::mutex> lock(lifecycle_mutex_);
            window = window_;
        }
        if (window) {
            PostMessageW(window, WM_CLOSE, 0, 0);
        }
        if (thread_.joinable()) {
            thread_.join();
        }
    }

private:
    static constexpr const wchar_t * window_class_name = L"SparkMoE.NotificationArea";

    struct snapshot {
        bool running;
        int port;
    };

    snapshot current_snapshot() {
        std::lock_guard<std::mutex> lock(state_.mutex);
        return {process_running_locked(state_), state_.router.port};
    }

    std::wstring tooltip_text(const snapshot & value) const {
        if (!value.running) {
            return L"Spark Edge - Router stopped";
        }
        return L"Spark Edge - Router running on port " + std::to_wstring(value.port);
    }

    bool add_icon(HWND window) {
        icon_data_ = {};
        icon_data_.cbSize = sizeof(icon_data_);
        icon_data_.hWnd = window;
        icon_data_.uID = 1;
        icon_data_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
        icon_data_.uCallbackMessage = tray_callback_message;
        icon_data_.hIcon = static_cast<HICON>(LoadImageW(
            GetModuleHandleW(nullptr), MAKEINTRESOURCEW(sparkmoe_icon_resource), IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
        if (!icon_data_.hIcon) {
            icon_data_.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
        }
        const std::wstring tooltip = tooltip_text(current_snapshot());
        lstrcpynW(icon_data_.szTip, tooltip.c_str(), ARRAYSIZE(icon_data_.szTip));
        if (!Shell_NotifyIconW(NIM_ADD, &icon_data_)) {
            return false;
        }
        icon_data_.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &icon_data_);
        icon_added_ = true;
        return true;
    }

    void remove_icon() {
        if (icon_added_) {
            Shell_NotifyIconW(NIM_DELETE, &icon_data_);
            icon_added_ = false;
        }
    }

    void update_tooltip() {
        if (!icon_added_) {
            return;
        }
        const std::wstring tooltip = tooltip_text(current_snapshot());
        icon_data_.uFlags = NIF_TIP | NIF_SHOWTIP;
        lstrcpynW(icon_data_.szTip, tooltip.c_str(), ARRAYSIZE(icon_data_.szTip));
        Shell_NotifyIconW(NIM_MODIFY, &icon_data_);
    }

    void show_error(const std::string & message) {
        {
            std::lock_guard<std::mutex> lock(state_.mutex);
            state_.last_error = message;
        }
        append_log(state_, message);
        MessageBoxW(window_, utf8_to_wide(message).c_str(), L"Spark Edge", MB_OK | MB_ICONERROR | MB_TASKMODAL);
    }

    void open_ui() {
        if (state_.open_ui) state_.open_ui();
    }

    void start_router() {
        try {
            {
                std::lock_guard<std::mutex> lock(state_.mutex);
                start_router_locked(state_);
            }
            append_log(state_, "Router started from the notification area");
        } catch (const std::exception & error) {
            show_error(std::string("Router start failed: ") + error.what());
        }
        update_tooltip();
    }

    void stop_router() {
        std::lock_guard<std::mutex> operation(state_.operation_mutex);
        if (state_.active_requests > 0) {
            show_error(state_.hebrew ? "שרת המודלים עדיין מטפל בבקשה." : "The model server is still handling a request.");
            return;
        }
        {
            std::lock_guard<std::mutex> lock(state_.mutex);
            stop_router_locked(state_);
        }
        append_log(state_, "Router stopped from the notification area");
        update_tooltip();
    }

    void exit_application() {
        append_log(state_, "SparkMoE exit requested from the notification area");
        {
            std::lock_guard<std::mutex> lock(state_.mutex);
            stop_router_locked(state_);
        }
        server_.stop();
        DestroyWindow(window_);
    }

    void show_menu() {
        const snapshot value = current_snapshot();
        HMENU menu = CreatePopupMenu();
        if (!menu) {
            return;
        }
        AppendMenuW(menu, MF_STRING | MF_DEFAULT, tray_command_open, state_.hebrew ? L"פתיחת Spark Edge" : L"Open Spark Edge");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        const std::wstring status = value.running ?
            L"Router: Running on port " + std::to_wstring(value.port) : L"Router: Stopped";
        AppendMenuW(menu, MF_STRING | MF_DISABLED | MF_GRAYED, 0, status.c_str());
        AppendMenuW(menu, MF_STRING | (value.running ? MF_GRAYED : 0), tray_command_start, state_.hebrew ? L"הפעלת שרת" : L"Start server");
        AppendMenuW(menu, MF_STRING | (value.running ? 0 : MF_GRAYED), tray_command_stop, state_.hebrew ? L"עצירת שרת" : L"Stop server");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, tray_command_exit, state_.hebrew ? L"יציאה מ־Spark Edge" : L"Exit Spark Edge");

        POINT point{};
        GetCursorPos(&point);
        SetForegroundWindow(window_);
        const UINT command = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
            point.x, point.y, 0, window_, nullptr);
        DestroyMenu(menu);
        PostMessageW(window_, WM_NULL, 0, 0);
        if (command) {
            handle_command(command);
        }
    }

    void handle_command(UINT command) {
        switch (command) {
            case tray_command_open:
                open_ui();
                break;
            case tray_command_start:
                start_router();
                break;
            case tray_command_stop:
                stop_router();
                break;
            case tray_command_exit:
                exit_application();
                break;
        }
    }

    LRESULT window_message(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == taskbar_created_message_) {
            icon_added_ = false;
            add_icon(window);
            return 0;
        }
        switch (message) {
            case tray_callback_message: {
                const UINT event = LOWORD(lparam);
                if (event == WM_CONTEXTMENU || event == WM_RBUTTONUP) {
                    show_menu();
                } else if (event == NIN_SELECT || event == NIN_KEYSELECT || event == WM_LBUTTONDBLCLK) {
                    open_ui();
                }
                return 0;
            }
            case WM_COMMAND:
                handle_command(LOWORD(wparam));
                return 0;
            case WM_TIMER:
                if (wparam == tray_timer_id) {
                    update_tooltip();
                }
                return 0;
            case WM_QUERYENDSESSION:
                return TRUE;
            case WM_ENDSESSION:
                if (wparam) {
                    server_.stop();
                }
                return 0;
            case WM_CLOSE:
                DestroyWindow(window);
                return 0;
            case WM_DESTROY:
                KillTimer(window, tray_timer_id);
                remove_icon();
                {
                    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
                    window_ = nullptr;
                }
                PostQuitMessage(0);
                return 0;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        notification_area * self = reinterpret_cast<notification_area *>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto * create = reinterpret_cast<const CREATESTRUCTW *>(lparam);
            self = static_cast<notification_area *>(create->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        return self ? self->window_message(window, message, wparam, lparam) :
            DefWindowProcW(window, message, wparam, lparam);
    }

    void run() {
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        taskbar_created_message_ = RegisterWindowMessageW(L"TaskbarCreated");
        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.lpfnWndProc = window_proc;
        window_class.hInstance = instance;
        window_class.lpszClassName = window_class_name;
        const ATOM registered = RegisterClassExW(&window_class);
        const bool class_ready = registered != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
        HWND window = class_ready ? CreateWindowExW(WS_EX_TOOLWINDOW, window_class_name, L"Spark Edge",
            WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr, instance, this) : nullptr;
        const bool ready = window && add_icon(window);
        if (ready) {
            SetTimer(window, tray_timer_id, 2000, nullptr);
        }
        {
            std::lock_guard<std::mutex> lock(lifecycle_mutex_);
            window_ = window;
            ready_ = ready;
            initialized_ = true;
        }
        lifecycle_condition_.notify_one();

        if (ready) {
            MSG message{};
            while (GetMessageW(&message, nullptr, 0, 0) > 0) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        } else if (window) {
            DestroyWindow(window);
        }
        if (registered) {
            UnregisterClassW(window_class_name, instance);
        }
    }

    app_state & state_;
    httplib::Server & server_;
    std::thread thread_;
    std::mutex lifecycle_mutex_;
    std::condition_variable lifecycle_condition_;
    HWND window_ = nullptr;
    NOTIFYICONDATAW icon_data_{};
    UINT taskbar_created_message_ = 0;
    bool initialized_ = false;
    bool ready_ = false;
    bool icon_added_ = false;
};
#endif

std::string request_origin(const httplib::Request & request) {
    return request.has_header("Origin") ? request.get_header_value("Origin") : std::string();
}

bool origin_allowed(const httplib::Request & request, app_state & state) {
    const auto host = lowercase(request.get_header_value("Host"));
    if (host != "127.0.0.1:" + std::to_string(manager_port()) && host != "localhost:" + std::to_string(manager_port())) return false;
    const std::string origin = request_origin(request);
    if (origin.empty()) {
        return true;
    }
    int port = 8080;
    int previous_port = 0;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        port = state.applied_router.port;
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
    response.set_header("Access-Control-Allow-Methods", "GET, POST, PATCH, DELETE, OPTIONS");
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

json router_models(int port) {
    httplib::Client client("127.0.0.1", port);
    client.set_connection_timeout(0, 300000);
    client.set_read_timeout(3);
    const auto response = client.Get("/models");
    if (!response || response->status != 200) return nullptr;
    const auto payload = json::parse(response->body, nullptr, false);
    if (!payload.is_object() || !payload.contains("data") || !payload["data"].is_array()) return nullptr;
    return payload["data"];
}

std::string model_status(const json & models, const std::string & id) {
    for (const auto & model : models) if (model.value("id", "") == id) return model.value("status", json::object()).value("value", "unloaded");
    return "unloaded";
}

// Call with operation_mutex held. Requests keep their applied settings until the next load.
void prepare_model(app_state & state, const std::string & requested, bool apply_pending) {
    std::string id;
    router_config router;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        id = canonical_model(state, requested);
        library_response_locked(state);
        const auto model = std::find_if(state.library.begin(), state.library.end(), [&](const json & row) { return row["id"] == id; });
        if (model == state.library.end()) throw std::runtime_error("Model not found in the library.");
        if (!model->value("available", false)) throw std::runtime_error("Model files are missing or incomplete. Update the model location.");
        if (!process_running_locked(state)) start_router_locked(state);
        router = state.applied_router;
    }
    httplib::Client client("127.0.0.1", router.port);
    client.set_connection_timeout(1);
    client.set_read_timeout(120);
    for (int attempt = 0; attempt < 100; ++attempt) {
        const auto health = client.Get("/health");
        if (health && health->status == 200) break;
        if (attempt == 99) throw std::runtime_error("The model server is still starting. Try again shortly.");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    const auto status = model_status(router_models(router.port), id);
    const bool running = status == "loaded" || status == "loading" || status == "sleeping";
    bool pending;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        pending = !state.applied_recipes.count(id) || state.applied_recipes.at(id) != recipe_locked(state, id);
    }
    if (running && (!apply_pending || !pending)) return;
    if (state.active_requests > 0) throw std::runtime_error("Wait for active requests to finish before loading or reloading a model.");
    std::map<std::string, model_profile> previous_profiles;
    std::map<std::string, json> previous_recipes;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        previous_profiles = state.applied_profiles;
        previous_recipes = state.applied_recipes;
        state.applied_profiles[id] = resolved_profile(state, id);
        state.applied_recipes[id] = recipe_locked(state, id);
        try { write_preset_locked(state); }
        catch (...) { state.applied_profiles = previous_profiles; state.applied_recipes = previous_recipes; throw; }
    }
    try {
        if (!reload_router_models(router)) throw std::runtime_error("The model server could not read the saved settings.");
        const auto reloaded_status = model_status(router_models(router.port), id);
        if (reloaded_status == "unloaded" || reloaded_status == "failed") {
            const auto response = client.Post("/models/load", json({{"model", id}}).dump(), "application/json");
            if (!response || response->status != 200) throw std::runtime_error("The model could not be loaded. Check Activity for details.");
        }
    } catch (...) {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.applied_profiles = previous_profiles;
        state.applied_recipes = previous_recipes;
        write_preset_locked(state);
        throw;
    }
}

json public_models(app_state & state, bool detailed) {
    int port;
    { std::lock_guard<std::mutex> lock(state.mutex); port = state.applied_router.port; }
    auto runtime = router_models(port);
    std::lock_guard<std::mutex> lock(state.mutex);
    const bool stale = runtime.is_null() && process_running_locked(state);
    if (stale) {
        runtime = state.last_model_status;
    } else {
        state.last_model_status = runtime.is_array() ? runtime : json::array();
    }
    library_response_locked(state);
    std::unordered_set<std::string> loaded;
    auto latest = state.application.value("last_loaded_model", std::string{});
    for (const auto & row : runtime) {
        const auto id = row.value("id", std::string{});
        const auto status = model_status(runtime, id);
        if (!id.empty() && (status == "loaded" || status == "sleeping")) {
            loaded.insert(id);
            if (!state.loaded_models.count(id)) latest = id;
        }
    }
    state.loaded_models = std::move(loaded);
    if (latest != state.application.value("last_loaded_model", std::string{})) {
        const auto previous = state.application["last_loaded_model"];
        state.application["last_loaded_model"] = latest;
        try { save_config_locked(state); }
        catch (const std::exception & error) {
            state.application["last_loaded_model"] = previous;
            append_log(state, error.what());
        }
    }
    json result = json::array();
    for (const auto & entry : state.library) {
        if (!entry.value("available", false)) continue;
        const auto id = entry["id"].get<std::string>();
        json model = {{"id", id}, {"object", "model"}, {"created", 0}, {"owned_by", "sparkmoe"},
                      {"status", {{"value", model_status(runtime, id)}}}};
        if (detailed) {
            for (const auto & current : runtime) if (current.value("id", "") == id) model = current;
            if (!model.contains("status")) model["status"] = {{"value", "unloaded"}};
            model["name"] = entry["display_name"];
            model["sparkmoe"] = {{"favorite", entry.value("favorite", false)}, {"is_default", state.application.value("default_model", "") == id}, {"last_loaded", state.application.value("last_loaded_model", "") == id}};
        }
        result.push_back(std::move(model));
    }
    return {{"object", "list"}, {"data", result}, {"models", result}, {"status_stale", stale}};
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
        state.configuration_warning = std::string("Saved settings could not be loaded. A recovery copy was kept: ") + error.what();
        state.configuration_read_only = std::string(error.what()).find("newer SparkMoE") != std::string::npos;
        if (std::filesystem::exists(state.config_path)) {
            std::filesystem::copy_file(state.config_path, state.data_directory / ("config-recovery-" + timestamp_string(true) + ".json"), std::filesystem::copy_options::skip_existing);
        }
        const auto config_path = state.config_path;
        const auto backup = std::filesystem::path(config_path.native() + std::filesystem::path(".bak").native());
        if (!state.configuration_read_only && std::filesystem::exists(backup)) {
            try {
                state.config_path = backup;
                load_config(state);
                state.configuration_warning = "Settings were restored from the last backup. A recovery copy of the unreadable file was kept.";
            } catch (...) {}
            state.config_path = config_path;
        }
        append_log(state, state.configuration_warning);
    }
    try {
        if (state.library.empty() && !state.router.model_directory.empty() && std::filesystem::is_directory(native_path(state.router.model_directory))) {
            merge_library_locked(state, sparkmoe::discover_models(native_path(state.router.model_directory)));
        }
        std::lock_guard<std::mutex> lock(state.mutex);
#if defined(_WIN32)
        state.engine_id = state.application.value("engine_id", std::string{});
        if (!state.engine_id.empty()) {
            try { start_router_locked(state); }
            catch (const std::exception & error) {
                append_log(state, error.what());
                state.engine_id.clear();
            }
        }
#endif
        if (!process_running_locked(state)) start_router_locked(state);
    } catch (const std::exception & error) {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.last_error = error.what();
        append_log(state, std::string("Router start failed: ") + error.what());
    }

    httplib::Server server;
    server.new_task_queue = []() { return new httplib::ThreadPool(16); };
    server.set_payload_max_length(64 * 1024 * 1024);
    auto authorize = [&](const httplib::Request & request, httplib::Response & response) {
        if (!origin_allowed(request, state)) {
            json_response(response, {{"error", "request origin is not allowed"}}, 403);
            return false;
        }
        set_cors_headers(request, response);
        return true;
    };

    server.Options(R"(/.*)", [&](const httplib::Request & request, httplib::Response & response) {
        if (authorize(request, response)) response.status = 204;
    });
    server.Post("/api/browse-model-file", [&](const httplib::Request & request, httplib::Response & response) {
        if (!authorize(request, response)) return;
        try { json_response(response, browse_models_directory("", true, state.native_window ? state.native_window() : nullptr)); }
        catch (const std::exception & error) { json_response(response, {{"error", error.what()}}, 400); }
    });
#if defined(_WIN32)
    const sparkmoe::engine_packages engines(state.data_directory / "engines");
    server.Get("/api/engines", [&](const httplib::Request & request, httplib::Response & response) {
        if (!authorize(request, response)) return;
        try {
            auto result = json{{"installed", engines.installed()}};
            {
                std::lock_guard<std::mutex> lock(state.mutex);
                result["active"] = state.engine_id;
            }
            if (request.get_param_value("check") == "1") result["available"] = engines.catalog();
            json_response(response, result);
        } catch (const std::exception & error) { json_response(response, {{"error", error.what()}}, 502); }
    });
    server.Post("/api/engines/download", [&](const httplib::Request & request, httplib::Response & response) {
        if (!authorize(request, response)) return;
        std::unique_lock<std::mutex> transfer(state.engine_download_mutex, std::try_to_lock);
        if (!transfer.owns_lock()) { json_response(response, {{"error", "An engine download is in progress."}}, 409); return; }
        try {
            engines.download(json::parse(request.body).at("id").get<std::string>());
            json_response(response, {{"installed", engines.installed()}});
        } catch (const std::exception & error) { json_response(response, {{"error", error.what()}}, 400); }
    });
    server.Post("/api/engines/apply", [&](const httplib::Request & request, httplib::Response & response) {
        if (!authorize(request, response)) return;
        std::lock_guard<std::mutex> operation(state.operation_mutex);
        if (state.active_requests > 0) { json_response(response, {{"error", "Wait for active requests to finish before switching engines."}}, 409); return; }
        try {
            const auto id = json::parse(request.body).at("id").get<std::string>();
            if (!id.empty()) engines.executable(id);
            std::string previous;
            std::string last_model;
            auto boot = [&](const std::string & next) {
                {
                    std::lock_guard<std::mutex> lock(state.mutex);
                    if (process_running_locked(state)) stop_router_locked(state);
                    state.engine_id = next;
                    start_router_locked(state);
                }
                bool ready = false;
                for (int attempt = 0; attempt < 40; ++attempt) {
                    {
                        std::lock_guard<std::mutex> lock(state.mutex);
                        if (!process_running_locked(state)) break;
                        if (router_ready_locked(state)) { ready = true; break; }
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
                }
                if (!ready) throw std::runtime_error("The engine did not pass its readiness check.");
                if (!last_model.empty()) {
                    prepare_model(state, last_model, false);
                    int port;
                    {
                        std::lock_guard<std::mutex> lock(state.mutex);
                        port = state.applied_router.port;
                    }
                    for (int attempt = 0; attempt < 120; ++attempt) {
                        const auto status = model_status(router_models(port), last_model);
                        if (status == "loaded" || status == "sleeping") return;
                        if (status == "failed") break;
                        std::this_thread::sleep_for(std::chrono::seconds(1));
                    }
                    throw std::runtime_error("The engine could not restore the active model.");
                }
            };
            {
                std::lock_guard<std::mutex> lock(state.mutex);
                previous = state.engine_id;
                if (!state.loaded_models.empty()) {
                    last_model = state.application.value("last_loaded_model", std::string{});
                    if (!state.loaded_models.count(last_model)) last_model.clear();
                }
            }
            try {
                boot(id);
                std::lock_guard<std::mutex> lock(state.mutex);
                state.application["engine_id"] = id;
                save_config_locked(state);
            } catch (const std::exception & original) {
                std::string recovery = previous;
                try { boot(previous); }
                catch (...) { recovery.clear(); boot(""); }
                {
                    std::lock_guard<std::mutex> lock(state.mutex);
                    state.application["engine_id"] = recovery;
                    save_config_locked(state);
                }
                json_response(response, {{"error", original.what()}, {"active", recovery}, {"rolled_back", true}}, 409);
                return;
            }
            json_response(response, {{"active", id}});
        } catch (const std::exception & error) { json_response(response, {{"error", error.what()}}, 400); }
    });
#endif

    sparkmoe::downloads downloads(state.data_directory, [&](const std::filesystem::path & path) {
        const auto models = sparkmoe::discover_models(path);
        std::lock_guard<std::mutex> operation(state.operation_mutex);
        router_config router;
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            merge_library_locked(state, models, true);
            write_preset_locked(state);
            router = state.applied_router;
        }
        if (state.active_requests == 0) reload_router_models(router);
    });
    auto download_action = [&](const std::string & action, const httplib::Request & request, httplib::Response & response) {
        if (!authorize(request, response)) return;
        try {
            auto input = request.body.empty() ? json::object() : json::parse(request.body);
            for (const auto & param : request.params) input[param.first] = param.second;
            json_response(response, downloads.dispatch(action, input));
        } catch (const std::exception & error) { json_response(response, {{"error", error.what()}}, 400); }
    };
    server.Get("/api/downloads", [&](const auto & request, auto & response) { download_action("list", request, response); });
    server.Post("/api/downloads", [&](const auto & request, auto & response) { download_action("add", request, response); });
    for (const std::string action : {"search", "catalog"}) server.Get("/api/downloads/" + action, [&, action](const auto & request, auto & response) { download_action(action, request, response); });
    for (const std::string action : {"pause", "resume"}) server.Post("/api/downloads/" + action, [&, action](const auto & request, auto & response) { download_action(action, request, response); });
    auto connection_action = [&](const std::string & action, const httplib::Request & request, httplib::Response & response) {
        if (!authorize(request, response)) return;
        try {
            sparkmoe::connection_context context;
            context.data_directory = state.data_directory;
            context.api_base_url = "http://127.0.0.1:" + std::to_string(manager_port()) + "/v1";
            context.api_key = "sparkmoe-local";
            {
                std::lock_guard<std::mutex> lock(state.mutex);
                library_response_locked(state);
                for (auto model : state.library) {
                    if (!model.value("available", false)) continue;
                    model["context_length"] = resolved_profile(state, model["id"].get<std::string>()).ctx_size;
                    model["supports_tools"] = model.value("metadata", json::object()).value("supports_tools", json(nullptr));
                    context.models.push_back(model);
                }
            }
            context.ensure_model_ready = [&](const std::string & id) {
                std::lock_guard<std::mutex> operation(state.operation_mutex);
                prepare_model(state, id, false);
                int port;
                { std::lock_guard<std::mutex> lock(state.mutex); port = state.applied_router.port; }
                for (int attempt = 0; attempt < 120; ++attempt) {
                    const auto status = model_status(router_models(port), id);
                    if (status == "loaded" || status == "sleeping") return;
                    if (status != "loading") throw std::runtime_error("The model did not load. Check Activity for details.");
                    if (request.is_connection_closed()) throw std::runtime_error("Connection launch was cancelled.");
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                }
                throw std::runtime_error("The model is still loading. Open the connection again when it is ready.");
            };
            json_response(response, sparkmoe::connections_dispatch(action, request.body.empty() ? json::object() : json::parse(request.body), context));
        } catch (const std::exception & error) { json_response(response, {{"error", error.what()}}, 400); }
    };
    server.Get("/api/connections", [&](const auto & request, auto & response) { connection_action("list", request, response); });
    for (const std::string action : {"preview", "configure", "launch", "remove"}) server.Post("/api/connections/" + action, [&, action](const auto & request, auto & response) { connection_action(action, request, response); });
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
            {
                std::lock_guard<std::mutex> lock(state.mutex);
                if (input.contains("revision") && input["revision"] != state.revision) {
                    json_response(response, {{"error", "Settings changed in another window. Reload them before saving."}}, 409);
                    return;
                }
                const router_config previous = state.router;
                const router_config next = router_config_from_json(input, previous);
                validate_router_config(next);
                state.router = next;
                ++state.revision;
                try { save_config_locked(state); }
                catch (...) { state.router = previous; --state.revision; throw; }
            }
            json_response(response, {{"ok", true}, {"port", manager_port()}, {"status", status_json(state)}});
        } catch (const std::exception & error) { json_response(response, {{"error", error.what()}}, 400); }
    });
    server.Get("/api/recommendation", [&](const httplib::Request & request, httplib::Response & response) {
        if (!authorize(request, response)) return;
        json_response(response, recommendation_json());
    });
    server.Post("/api/browse-model-directory", [&](const httplib::Request & request, httplib::Response & response) {
        if (!authorize(request, response)) return;
        try {
            std::string initial_directory;
            {
                std::lock_guard<std::mutex> lock(state.mutex);
                initial_directory = state.router.model_directory;
            }
            json_response(response, browse_models_directory(initial_directory, false, state.native_window ? state.native_window() : nullptr));
        } catch (const std::exception & error) {
            json_response(response, {{"error", error.what()}}, 500);
        }
    });
    server.Get("/api/library", [&](const httplib::Request & request, httplib::Response & response) {
        if (!authorize(request, response)) return;
        try {
            if (request.get_param_value("rescan") == "1") {
                std::string directory;
                { std::lock_guard<std::mutex> lock(state.mutex); directory = state.router.model_directory; }
                if (!directory.empty()) {
                    auto discovered = sparkmoe::discover_models(native_path(directory));
                    std::lock_guard<std::mutex> lock(state.mutex);
                    merge_library_locked(state, discovered);
                    write_preset_locked(state);
                }
            }
            std::lock_guard<std::mutex> lock(state.mutex);
            json_response(response, library_response_locked(state));
        } catch (const std::exception & error) { json_response(response, {{"error", error.what()}}, 400); }
    });
    server.Post("/api/library", [&](const httplib::Request & request, httplib::Response & response) {
        if (!authorize(request, response)) return;
        try {
            std::lock_guard<std::mutex> operation(state.operation_mutex);
            const auto input = json::parse(request.body);
            const auto discovered = sparkmoe::discover_models(native_path(input.value("path", std::string())));
            router_config router;
            {
                std::lock_guard<std::mutex> lock(state.mutex);
                merge_library_locked(state, discovered, true);
                write_preset_locked(state);
                router = state.applied_router;
            }
            if (state.active_requests == 0) reload_router_models(router);
            std::lock_guard<std::mutex> lock(state.mutex);
            json_response(response, library_response_locked(state));
        } catch (const std::exception & error) { json_response(response, {{"error", error.what()}}, 400); }
    });
    server.Patch("/api/library", [&](const httplib::Request & request, httplib::Response & response) {
        if (!authorize(request, response)) return;
        try {
            std::lock_guard<std::mutex> operation(state.operation_mutex);
            const auto input = json::parse(request.body);
            if (input.contains("path") && state.active_requests > 0) throw std::runtime_error("Wait for active requests to finish before moving a model.");
            json relocated;
            if (input.contains("path")) relocated = sparkmoe::discover_models(native_path(input["path"].get<std::string>()));
            std::lock_guard<std::mutex> lock(state.mutex);
            const auto previous = state.library;
            const auto id = input.value("id", std::string());
            auto model = std::find_if(state.library.begin(), state.library.end(), [&](const json & entry) { return entry["id"] == id; });
            if (model == state.library.end()) throw std::runtime_error("Model not found.");
            if (!relocated.is_null()) {
                if (relocated.size() != 1) throw std::runtime_error("Choose one model file to update its location.");
                auto replacement = relocated[0];
                replacement["id"] = (*model)["id"];
                replacement["display_name"] = (*model)["display_name"];
                replacement["favorite"] = (*model)["favorite"];
                replacement["legacy_ids"] = (*model)["legacy_ids"];
                *model = replacement;
            }
            if (input.contains("display_name")) {
                const auto name = input["display_name"].get<std::string>();
                if (name.empty() || name.size() > 160 || name.find_first_of("\r\n") != std::string::npos) throw std::runtime_error("Enter a model name of up to 160 characters.");
                (*model)["display_name"] = name;
            }
            if (input.contains("favorite")) (*model)["favorite"] = input["favorite"].get<bool>();
            ++state.revision;
            try { save_config_locked(state); write_preset_locked(state); }
            catch (...) { state.library = previous; --state.revision; throw; }
            json_response(response, library_response_locked(state));
        } catch (const std::exception & error) { json_response(response, {{"error", error.what()}}, 400); }
    });
    server.Delete("/api/library", [&](const httplib::Request & request, httplib::Response & response) {
        if (!authorize(request, response)) return;
        try {
            std::lock_guard<std::mutex> operation(state.operation_mutex);
            if (state.active_requests > 0) { json_response(response, {{"error", "Wait for active requests to finish before removing a model."}}, 409); return; }
            router_config router;
            {
                std::lock_guard<std::mutex> lock(state.mutex);
                const auto id = request.get_param_value("id");
                auto model = std::find_if(state.library.begin(), state.library.end(), [&](const json & entry) { return entry["id"] == id; });
                if (model == state.library.end()) throw std::runtime_error("Model not found.");
                const auto previous_library = state.library;
                const auto previous_application = state.application;
                const auto path = (*model)["path"].get<std::string>();
                const bool added = state.removed_paths.insert(path).second;
                state.library.erase(model);
                for (const char * field : {"default_model", "last_loaded_model"}) {
                    if (state.application.value(field, "") == id) state.application[field] = "";
                }
                ++state.revision;
                try {
                    write_preset_locked(state);
                    save_config_locked(state);
                } catch (...) {
                    state.library = previous_library;
                    state.application = previous_application;
                    if (added) state.removed_paths.erase(path);
                    --state.revision;
                    write_preset_locked(state);
                    throw;
                }
                router = state.applied_router;
            }
            if (state.active_requests == 0) reload_router_models(router);
            std::lock_guard<std::mutex> lock(state.mutex);
            json_response(response, library_response_locked(state));
        } catch (const std::exception & error) { json_response(response, {{"error", error.what()}}, 400); }
    });
    server.Get("/api/application", [&](const httplib::Request & request, httplib::Response & response) {
        if (!authorize(request, response)) return;
        std::lock_guard<std::mutex> lock(state.mutex);
        auto result = state.application;
        result["revision"] = state.revision;
        json_response(response, result);
    });
    server.Post("/api/application", [&](const httplib::Request & request, httplib::Response & response) {
        if (!authorize(request, response)) return;
        try {
            const auto input = json::parse(request.body);
            std::lock_guard<std::mutex> lock(state.mutex);
            auto next = state.application;
            for (const auto & field : input.items()) {
                if (field.key() == "revision") {
                    if (field.value() != state.revision) { json_response(response, {{"error", "Settings changed in another window."}}, 409); return; }
                } else if (field.key() == "background_mode" || field.key() == "launch_at_login") next[field.key()] = field.value().get<bool>();
                else if (field.key() == "locale") {
                    const auto locale = field.value().get<std::string>();
                    if (locale != "he" && locale != "en") throw std::runtime_error("Choose Hebrew or English.");
                    next[field.key()] = locale;
                }
                else if (field.key() == "default_model") next[field.key()] = field.value().get<std::string>();
                else throw std::runtime_error("Unknown application setting.");
            }
            const auto default_model = next.value("default_model", "");
            if (!default_model.empty() && std::none_of(state.library.begin(), state.library.end(), [&](const auto & model) { return model["id"] == default_model; })) throw std::runtime_error("Default model is not in the library.");
            const auto previous = state.application;
            auto startup = [&](bool enabled) {
#if defined(_WIN32)
                HKEY key = nullptr;
                if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) throw std::runtime_error("Cannot update Windows startup settings.");
                LSTATUS result = ERROR_SUCCESS;
                if (enabled) {
                    const auto command = quote_windows_arg((executable_directory() / L"SparkMoE.exe").wstring());
                    result = RegSetValueExW(key, L"SparkMoE", 0, REG_SZ, reinterpret_cast<const BYTE *>(command.c_str()), static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
                } else { result = RegDeleteValueW(key, L"SparkMoE"); if (result == ERROR_FILE_NOT_FOUND) result = ERROR_SUCCESS; }
                RegCloseKey(key);
                if (result != ERROR_SUCCESS) throw std::runtime_error("Cannot update Windows startup settings.");
#else
                if (enabled) throw std::runtime_error("Launch at sign-in is available on Windows.");
#endif
            };
            const bool startup_changed = previous["launch_at_login"] != next["launch_at_login"];
            if (startup_changed) startup(next.value("launch_at_login", false));
            state.application = next;
            ++state.revision;
            try { save_config_locked(state); }
            catch (...) {
                state.application = previous;
                --state.revision;
                if (startup_changed) startup(previous.value("launch_at_login", false));
                throw;
            }
            state.background_mode = next.value("background_mode", false);
            state.hebrew = next.value("locale", "he") == std::string("he");
            next["revision"] = state.revision;
            json_response(response, next);
        } catch (const std::exception & error) { json_response(response, {{"error", error.what()}}, 400); }
    });
    server.Get("/api/model-profile", [&](const httplib::Request & request, httplib::Response & response) {
        if (!authorize(request, response)) return;
        try {
            const auto model = request.get_param_value("model");
            validate_model_id(model);
            std::lock_guard<std::mutex> lock(state.mutex);
            json_response(response, model_profile_response(state, model));
        } catch (const std::exception & error) { json_response(response, {{"error", error.what()}}, 400); }
    });
    auto save_profile = [&](const httplib::Request & request, httplib::Response & response) {
        if (!authorize(request, response)) return;
        try {
            const bool reset = request.method == "DELETE";
            const json input = reset ? json::object() : json::parse(request.body);
            const auto requested = reset ? request.get_param_value("model") : input.value("model", std::string());
            validate_model_id(requested);
            std::lock_guard<std::mutex> lock(state.mutex);
            const auto model = canonical_model(state, requested);
            const json revision = reset && request.has_param("revision") ? json(std::stoull(request.get_param_value("revision"))) : input.value("revision", json(nullptr));
            if (!revision.is_null() && revision != state.profile_revisions[model]) {
                json_response(response, {{"error", "Model settings changed in another window. Reload them before saving."}}, 409);
                return;
            }
            const auto previous = state.overrides;
            if (reset) state.overrides.erase(model);
            else {
                const json changes = input.value("profile", json::object());
                if (!changes.is_object()) throw std::runtime_error("Model settings must be an object.");
                auto recipe = recipe_locked(state, model);
                for (const auto & field : changes.items()) {
                    if (!recipe.contains(field.key())) throw std::runtime_error("Unknown model setting: " + field.key());
                    recipe[field.key()] = field.value();
                }
                validate_profile(profile_from_json(recipe, state.default_profile));
                json overrides = state.overrides.count(model) ? state.overrides[model] : json::object();
                overrides.update(changes);
                state.overrides[model] = overrides;
            }
            ++state.profile_revisions[model];
            ++state.revision;
            try { save_config_locked(state); }
            catch (...) { state.overrides = previous; --state.profile_revisions[model]; --state.revision; throw; }
            auto result = model_profile_response(state, model);
            result["ok"] = true;
            result["router_reloaded"] = false;
            json_response(response, result);
        } catch (const std::exception & error) { json_response(response, {{"error", error.what()}}, 400); }
    };
    server.Post("/api/model-profile", save_profile);
    server.Delete("/api/model-profile", save_profile);
    server.Post("/api/model-apply", [&](const httplib::Request & request, httplib::Response & response) {
        if (!authorize(request, response)) return;
        try {
            const auto input = json::parse(request.body);
            std::lock_guard<std::mutex> operation(state.operation_mutex);
            const auto model = input.value("model", std::string());
            prepare_model(state, model, true);
            std::lock_guard<std::mutex> lock(state.mutex);
            json_response(response, model_profile_response(state, model));
        } catch (const std::exception & error) { json_response(response, {{"error", error.what()}}, 400); }
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
            std::lock_guard<std::mutex> operation(state.operation_mutex);
            if (state.active_requests > 0) { json_response(response, {{"error", "Wait for active requests to finish before restarting the server."}}, 409); return; }
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
        std::lock_guard<std::mutex> operation(state.operation_mutex);
        if (state.active_requests > 0) { json_response(response, {{"error", "Wait for active requests to finish before stopping the server."}}, 409); return; }
        std::lock_guard<std::mutex> lock(state.mutex);
        stop_router_locked(state);
        json_response(response, {{"ok", true}});
    });
    server.Post("/api/shutdown", [&](const httplib::Request & request, httplib::Response & response) {
        if (!authorize(request, response)) return;
        append_log(state, "SparkMoE shutdown requested");
        json_response(response, {{"ok", true}});
        server.stop();
    });
    server.Post("/api/open", [&](const httplib::Request & request, httplib::Response & response) {
        if (!authorize(request, response)) return;
        if (state.open_ui) state.open_ui();
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
                {"profile_count", state.overrides.size()},
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
    auto proxy = [&](const httplib::Request & request, httplib::Response & response) {
        if (!origin_allowed(request, state)) { json_response(response, {{"error", "Request origin is not allowed."}}, 403); return; }
        set_cors_headers(request, response);
        if (request.method == "GET" && sparkmoe::serve_ui_asset(request, response)) return;
        if (request.path.rfind("/api/", 0) == 0) { json_response(response, {{"error", "Unknown management endpoint."}}, 404); return; }
        try {
            if (request.method == "GET" && (request.path == "/v1/models" || request.path == "/models")) {
                json_response(response, public_models(state, request.path == "/models"));
                return;
            }
            const bool generation = request.method == "POST" &&
                (request.path.find("complet") != std::string::npos || request.path == "/v1/responses" || request.path == "/responses" || request.path == "/v1/messages");
            const bool control = request.path == "/models/load" || request.path == "/models/unload";
            httplib::Request forwarded = request;
            std::unique_lock<std::mutex> operation(state.operation_mutex, std::defer_lock);
            if (generation || control) operation.lock();
            if (generation || request.path == "/models/load") {
                auto body = json::parse(request.body);
                std::string model = body.value("model", "");
                if (model.empty()) {
                    std::lock_guard<std::mutex> lock(state.mutex);
                    model = state.application.value("default_model", "");
                    if (model.empty() && state.library.size() == 1) model = state.library[0]["id"].get<std::string>();
                }
                if (generation) {
                    bool autoload;
                    int engine_port;
                    { std::lock_guard<std::mutex> lock(state.mutex); autoload = state.applied_router.models_autoload; engine_port = state.applied_router.port; model = canonical_model(state, model); }
                    const auto status = model_status(router_models(engine_port), model);
                    if (!autoload && status != "loaded" && status != "loading" && status != "sleeping") throw std::runtime_error("Automatic model loading is disabled. Load the model in SparkMoE first.");
                }
                prepare_model(state, model, request.path == "/models/load");
                { std::lock_guard<std::mutex> lock(state.mutex); body["model"] = canonical_model(state, model); }
                forwarded.body = body.dump();
                if (request.path == "/models/load") { json_response(response, {{"success", true}}); return; }
            } else if (control && state.active_requests > 0) {
                json_response(response, {{"error", "Wait for active requests to finish before unloading the model."}}, 409);
                return;
            }
            int port;
            { std::lock_guard<std::mutex> lock(state.mutex); port = state.applied_router.port; }
            if (request.path == "/models/unload" && request.method == "POST") {
                const auto body = json::parse(request.body);
                std::string id;
                { std::lock_guard<std::mutex> lock(state.mutex); id = canonical_model(state, body.value("model", "")); }
                httplib::Client client("127.0.0.1", port);
                client.set_connection_timeout(1);
                client.set_read_timeout(30);
                const auto result = client.Post("/models/unload", json({{"model", id}}).dump(), "application/json");
                if (!result || result->status != 200) throw std::runtime_error("The model could not be unloaded.");
                for (int attempt = 0; attempt < 300; ++attempt) {
                    if (model_status(router_models(port), id) == "unloaded") { json_response(response, {{"success", true}}); return; }
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                throw std::runtime_error("The model is still unloading. Try again shortly.");
            }
            sparkmoe::proxy_request(forwarded, response, port, generation ? &state.active_requests : nullptr);
        } catch (const std::exception & error) { json_response(response, {{"error", {{"message", error.what()}, {"type", "invalid_request_error"}}}}, 400); }

    };
    server.Get(R"(/.*)", proxy);
    server.Post(R"(/.*)", proxy);
    server.Delete(R"(/.*)", proxy);
    server.Patch(R"(/.*)", proxy);
    server.Put(R"(/.*)", proxy);
    server.set_exception_handler([](const auto &, auto & response, std::exception_ptr error) {
        try {
            if (error) std::rethrow_exception(error);
        } catch (const std::exception & exception) {
            json_response(response, {{"error", exception.what()}}, 500);
        }
    });

    sparkmoe::desktop_window desktop;
    std::atomic<bool> desktop_ready{false};
    const std::string initial_route = state.library.empty() ? "/#/sparkmoe" : "/";
    state.native_window = [&]() { return desktop.native_handle(); };
    state.open_ui = [&]() { if (!desktop_ready || !desktop.show()) open_browser(manager_port(), initial_route); };
    if (!server.bind_to_port("127.0.0.1", manager_port())) {
        append_log(state, "The SparkMoE application port is already in use.");
        std::lock_guard<std::mutex> lock(state.mutex);
        stop_router_locked(state);
        return 1;
    }
#if defined(_WIN32)
    notification_area tray(state, server);
    const bool tray_enabled = std::getenv("SPARKMOE_NO_TRAY") == nullptr;
    if (tray_enabled) append_log(state, tray.start() ? "Notification area icon ready" : "Notification area icon unavailable");
#endif
    std::thread window_thread([&]() {
        if (std::getenv("SPARKMOE_NO_BROWSER")) return;
        for (int attempt = 0; attempt < 100 && !server.is_running(); ++attempt) std::this_thread::sleep_for(std::chrono::milliseconds(25));
        if (!server.is_running()) return;
        sparkmoe::desktop_window::options options;
        options.url = "http://127.0.0.1:" + std::to_string(manager_port()) + initial_route;
        options.user_data_directory = state.data_directory / "WebView2";
        options.keep_in_background = [&]() { return state.background_mode.load(); };
        options.use_hebrew = [&]() { return state.hebrew.load(); };
        options.has_active_requests = [&]() { return state.active_requests.load() > 0; };
        options.request_shutdown = [&]() { server.stop(); };
        options.log = [&](const std::string & message) { append_log(state, message); };
        options.browser_fallback = [&]() { open_browser(manager_port(), initial_route); };
        desktop_ready = std::getenv("SPARKMOE_BROWSER_ONLY") == nullptr && desktop.start(std::move(options));
        if (!desktop_ready) open_browser(manager_port(), initial_route);
    });
    const bool ok = server.listen_after_bind();
    if (window_thread.joinable()) window_thread.join();
    desktop.stop();
#if defined(_WIN32)
    if (tray_enabled) tray.stop();
#endif
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        stop_router_locked(state);
    }
    return ok ? 0 : 1;
}

} // namespace

#if defined(_WIN32)
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    SetCurrentProcessExplicitAppUserModelID(L"SparkMoE.Desktop");
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
        client.Post("/api/open", "{}", "application/json");
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
