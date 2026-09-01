// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright (c) 2026 SparkMoE contributors.

#include "httplib.h"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
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

constexpr int manager_port = 18080;

#include "dashboard.generated.h"

bool model_path_is_valid(const std::string & path);

struct server_config {
    std::string model_directory;
    std::string model;
    std::string mode = "auto";
    int cache = 2048;
    int ple_cache_mib = 64;
    int slots = 0;
    int threads = 4;
    std::string io_mode = "buffered";
    std::string cache_policy = "lru";
    std::string overlap = "off";
    std::string prefetch = "off";
    std::string dense_residency = "auto";
    int paged_layers = 0;
    int inference_threads = 0;
    std::string cpu_repack = "auto";
    int port = 8080;
    int ctx_size = 0;
    int batch_size = 2048;
    int ubatch_size = 512;
    int parallel = 1;
    int ctx_checkpoints = 4;
    int cache_ram_mib = 0;
    std::string cache_type_k = "f16";
    std::string cache_type_v = "f16";
    bool context_shift = true;
    bool mtp_enabled = false;
    std::string mtp_model;
    int mtp_cache_mib = 512;
    int mtp_draft_tokens = 3;
    int mtp_min_tokens = 0;
    double mtp_p_min = 0.0;
    int mtp_threads = 0;
    bool mtp_backend_sampling = true;
};

struct process_state {
    std::mutex mutex;
    server_config config;
    std::string log_path = "sparkmoe-server.log";
    std::string last_error;
    int64_t started_at_ms = 0;
    int last_exit_code = 0;
    bool has_exit_code = false;
#if defined(_WIN32)
    HANDLE process = nullptr;
    DWORD pid = 0;
#else
    pid_t pid = -1;
#endif
};

json config_json(const server_config & config, bool include_model = true) {
    json result = {
        {"model_directory", config.model_directory},
        {"mode", config.mode},
        {"cache", config.cache},
        {"ple_cache_mib", config.ple_cache_mib},
        {"slots", config.slots},
        {"threads", config.threads},
        {"io_mode", config.io_mode},
        {"cache_policy", config.cache_policy},
        {"overlap", config.overlap},
        {"prefetch", config.prefetch},
        {"dense_residency", config.dense_residency},
        {"paged_layers", config.paged_layers},
        {"inference_threads", config.inference_threads},
        {"cpu_repack", config.cpu_repack},
        {"port", config.port},
        {"ctx_size", config.ctx_size},
        {"batch_size", config.batch_size},
        {"ubatch_size", config.ubatch_size},
        {"parallel", config.parallel},
        {"ctx_checkpoints", config.ctx_checkpoints},
        {"cache_ram_mib", config.cache_ram_mib},
        {"cache_type_k", config.cache_type_k},
        {"cache_type_v", config.cache_type_v},
        {"context_shift", config.context_shift},
        {"mtp_enabled", config.mtp_enabled},
        {"mtp_cache_mib", config.mtp_cache_mib},
        {"mtp_draft_tokens", config.mtp_draft_tokens},
        {"mtp_min_tokens", config.mtp_min_tokens},
        {"mtp_p_min", config.mtp_p_min},
        {"mtp_threads", config.mtp_threads},
        {"mtp_backend_sampling", config.mtp_backend_sampling},
    };
    if (include_model) {
        result["model"] = config.model;
        result["mtp_model"] = config.mtp_model;
    }
    return result;
}

server_config config_from_json(const json & input, const server_config & base) {
    server_config config = base;
    config.model_directory = input.value("model_directory", config.model_directory);
    config.model = input.value("model", config.model);
    config.mode = input.value("mode", config.mode);
    config.cache = input.value("cache", config.cache);
    config.ple_cache_mib = input.value("ple_cache_mib", config.ple_cache_mib);
    config.slots = input.value("slots", config.slots);
    config.threads = input.value("threads", config.threads);
    config.io_mode = input.value("io_mode", config.io_mode);
    config.cache_policy = input.value("cache_policy", config.cache_policy);
    config.overlap = input.value("overlap", config.overlap);
    config.prefetch = input.value("prefetch", config.prefetch);
    config.dense_residency = input.value("dense_residency", config.dense_residency);
    config.paged_layers = input.value("paged_layers", config.paged_layers);
    config.inference_threads = input.value("inference_threads", config.inference_threads);
    config.cpu_repack = input.value("cpu_repack", config.cpu_repack);
    config.port = input.value("port", config.port);
    config.ctx_size = input.value("ctx_size", config.ctx_size);
    config.batch_size = input.value("batch_size", config.batch_size);
    config.ubatch_size = input.value("ubatch_size", config.ubatch_size);
    config.parallel = input.value("parallel", config.parallel);
    config.ctx_checkpoints = input.value("ctx_checkpoints", config.ctx_checkpoints);
    config.cache_ram_mib = input.value("cache_ram_mib", config.cache_ram_mib);
    config.cache_type_k = input.value("cache_type_k", config.cache_type_k);
    config.cache_type_v = input.value("cache_type_v", config.cache_type_v);
    config.context_shift = input.value("context_shift", config.context_shift);
    config.mtp_enabled = input.value("mtp_enabled", config.mtp_enabled);
    config.mtp_model = input.value("mtp_model", config.mtp_model);
    config.mtp_cache_mib = input.value("mtp_cache_mib", config.mtp_cache_mib);
    config.mtp_draft_tokens = input.value("mtp_draft_tokens", config.mtp_draft_tokens);
    config.mtp_min_tokens = input.value("mtp_min_tokens", config.mtp_min_tokens);
    config.mtp_p_min = input.value("mtp_p_min", config.mtp_p_min);
    config.mtp_threads = input.value("mtp_threads", config.mtp_threads);
    config.mtp_backend_sampling = input.value("mtp_backend_sampling", config.mtp_backend_sampling);
    return config;
}

void validate_config(const server_config & config) {
    static const std::unordered_set<std::string> cache_types = {
        "f16", "bf16", "q8_0", "q4_0", "q4_1", "iq4_nl", "q5_0", "q5_1",
    };
    if (!model_path_is_valid(config.model)) {
        throw std::runtime_error("model must be a readable local .gguf file");
    }
    if (config.mtp_enabled && !config.mtp_model.empty() && !model_path_is_valid(config.mtp_model)) {
        throw std::runtime_error("MTP sidecar must be a readable local .gguf file");
    }
    if (config.ple_cache_mib < 1 || config.ple_cache_mib > 4096 || config.mtp_cache_mib < 1 || config.mtp_cache_mib > 65536) {
        throw std::runtime_error("invalid PLE or MTP cache budget");
    }
    if (config.mode != "explicit" && config.mode != "auto" && config.mode != "os" && config.mode != "off") {
        throw std::runtime_error("invalid paging mode");
    }
    if ((config.io_mode != "buffered" && config.io_mode != "direct") ||
            (config.cache_policy != "lru" && config.cache_policy != "hybrid") ||
            (config.overlap != "on" && config.overlap != "off") ||
            (config.prefetch != "off" && config.prefetch != "temporal") ||
            (config.dense_residency != "auto" && config.dense_residency != "mmap" &&
             config.dense_residency != "resident" && config.dense_residency != "locked") ||
            config.paged_layers < 0 || config.paged_layers > 4096) {
        throw std::runtime_error("invalid engine policy");
    }
    if (config.cpu_repack != "auto" && config.cpu_repack != "on" && config.cpu_repack != "off") {
        throw std::runtime_error("invalid CPU repack mode");
    }
    if (config.cache < 0 || config.slots < 0 || config.threads < 1 || config.threads > 128 ||
            config.inference_threads < 0 || config.inference_threads > 128 ||
            config.port < 1024 || config.port > 65535) {
        throw std::runtime_error("invalid paging or network configuration");
    }
    if ((config.mode == "explicit" || config.mode == "auto") && config.cache == 0 && config.slots == 0) {
        throw std::runtime_error("explicit or auto mode requires a cache budget or slot count");
    }
    if ((config.mode == "explicit" || config.mode == "auto") && config.parallel != 1) {
        throw std::runtime_error("explicit expert paging currently requires one parallel chat slot");
    }
    if (config.ctx_size < 0 || config.ctx_size > 1048576 || config.batch_size < 1 || config.batch_size > 16384 ||
            config.ubatch_size < 1 || config.ubatch_size > config.batch_size || config.parallel < 1 ||
            config.parallel > 32 || config.ctx_checkpoints < 0 || config.ctx_checkpoints > 128 ||
            config.cache_ram_mib < -1 || config.cache_ram_mib > 1048576) {
        throw std::runtime_error("invalid context configuration");
    }
    if (cache_types.count(config.cache_type_k) == 0 || cache_types.count(config.cache_type_v) == 0) {
        throw std::runtime_error("unsupported KV cache type");
    }
    if (config.mtp_draft_tokens < 1 || config.mtp_draft_tokens > 64 || config.mtp_min_tokens < 0 ||
            config.mtp_min_tokens > config.mtp_draft_tokens || config.mtp_p_min < 0.0 ||
            config.mtp_p_min > 1.0 || config.mtp_threads < 0 || config.mtp_threads > 128) {
        throw std::runtime_error("invalid MTP configuration");
    }
}

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
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

std::wstring executable_directory() {
    std::vector<wchar_t> path(32768);
    const DWORD count = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (count == 0 || count >= path.size()) {
        throw std::runtime_error("cannot determine launcher directory");
    }
    std::wstring value(path.data(), count);
    return value.substr(0, value.find_last_of(L"\\/"));
}
#endif

std::string server_log_path() {
#if defined(_WIN32)
    return wide_to_utf8(executable_directory() + L"\\sparkmoe-server.log");
#else
    char path[4096] = {};
    return getcwd(path, sizeof(path)) ? std::string(path) + "/sparkmoe-server.log" : "sparkmoe-server.log";
#endif
}

std::string dashboard_file_path() {
#if defined(_WIN32)
    return wide_to_utf8(executable_directory() + L"\\dashboard.html");
#else
    char path[4096] = {};
    return getcwd(path, sizeof(path)) ? std::string(path) + "/dashboard.html" : "dashboard.html";
#endif
}

std::string load_dashboard() {
#if defined(_WIN32)
    std::ifstream file(std::filesystem::path(executable_directory() + L"\\dashboard.html"), std::ios::binary);
#else
    std::ifstream file(dashboard_file_path(), std::ios::binary);
#endif
    if (!file) {
        return dashboard_html;
    }
    std::ostringstream content;
    content << file.rdbuf();
    return content.str();
}

std::string load_brand_asset(const std::string & name) {
#if defined(_WIN32)
    const std::filesystem::path path = std::filesystem::path(executable_directory()) / L"assets" / L"branding" / utf8_to_wide(name);
    std::ifstream file(path, std::ios::binary);
#else
    std::ifstream file(std::filesystem::path("assets/branding") / name, std::ios::binary);
#endif
    if (!file) {
        return {};
    }
    std::ostringstream content;
    content << file.rdbuf();
    return content.str();
}

bool model_path_is_valid(const std::string & path) {
    if (lowercase(path).size() < 5 || lowercase(path).substr(path.size() - 5) != ".gguf") {
        return false;
    }
#if defined(_WIN32)
    const DWORD attrs = GetFileAttributesW(utf8_to_wide(path).c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
#else
    return access(path.c_str(), R_OK) == 0;
#endif
}

std::filesystem::path native_path(const std::string & path) {
#if defined(_WIN32)
    return std::filesystem::path(utf8_to_wide(path));
#else
    return std::filesystem::path(path);
#endif
}

std::string utf8_path(const std::filesystem::path & path) {
#if defined(_WIN32)
    return wide_to_utf8(path.wstring());
#else
    return path.string();
#endif
}

json models_in_directory(const std::string & directory) {
    if (directory.empty()) {
        throw std::runtime_error("choose a models directory first");
    }
    const std::filesystem::path root = native_path(directory);
    std::error_code error;
    if (!std::filesystem::is_directory(root, error) || error) {
        throw std::runtime_error("models directory does not exist or is not readable");
    }

    std::vector<json> models;
    const auto options = std::filesystem::directory_options::skip_permission_denied;
    for (std::filesystem::recursive_directory_iterator it(root, options, error), end; it != end; it.increment(error)) {
        if (error) {
            error.clear();
            continue;
        }
        if (!it->is_regular_file(error) || error || lowercase(utf8_path(it->path().extension())) != ".gguf") {
            error.clear();
            continue;
        }
        const std::filesystem::path relative = it->path().lexically_relative(root);
        const uintmax_t size = it->file_size(error);
        models.push_back({
            {"name", utf8_path(relative)},
            {"path", utf8_path(it->path())},
            {"size_bytes", error ? 0 : size},
        });
        error.clear();
        if (models.size() >= 2000) {
            break;
        }
    }
    std::sort(models.begin(), models.end(), [](const json & left, const json & right) {
        return lowercase(left["name"].get<std::string>()) < lowercase(right["name"].get<std::string>());
    });
    return {
        {"directory", directory},
        {"count", models.size()},
        {"models", models},
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
    if (SUCCEEDED(result)) {
        result = dialog->GetOptions(&options);
    }
    if (SUCCEEDED(result)) {
        result = dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM |
            FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR | FOS_DONTADDTORECENT);
    }
    if (SUCCEEDED(result)) {
        result = dialog->SetTitle(L"Choose the folder that contains your GGUF models");
    }
    if (SUCCEEDED(result)) {
        result = dialog->Show(nullptr);
    }
    if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        dialog->Release();
        if (uninitialize) {
            CoUninitialize();
        }
        return {{"cancelled", true}};
    }

    IShellItem * item = nullptr;
    if (SUCCEEDED(result)) {
        result = dialog->GetResult(&item);
    }
    PWSTR path = nullptr;
    if (SUCCEEDED(result)) {
        result = item->GetDisplayName(SIGDN_FILESYSPATH, &path);
    }
    const std::wstring selected = SUCCEEDED(result) && path ? path : L"";
    if (path) {
        CoTaskMemFree(path);
    }
    if (item) {
        item->Release();
    }
    if (dialog) {
        dialog->Release();
    }
    if (uninitialize) {
        CoUninitialize();
    }
    if (FAILED(result) || selected.empty()) {
        throw std::runtime_error("the selected folder does not have a local filesystem path");
    }
    return {{"cancelled", false}, {"directory", wide_to_utf8(selected)}};
#else
    throw std::runtime_error("native folder selection is currently available on Windows only");
#endif
}

bool process_running_locked(process_state & state) {
#if defined(_WIN32)
    if (!state.process) {
        return false;
    }
    DWORD code = 0;
    if (!GetExitCodeProcess(state.process, &code) || code != STILL_ACTIVE) {
        if (code != STILL_ACTIVE) {
            state.last_exit_code = static_cast<int>(code);
            state.has_exit_code = true;
        }
        CloseHandle(state.process);
        state.process = nullptr;
        state.pid = 0;
        return false;
    }
    return true;
#else
    if (state.pid <= 0) {
        return false;
    }
    int status = 0;
    const pid_t result = waitpid(state.pid, &status, WNOHANG);
    if (result == state.pid || (result < 0 && errno == ECHILD)) {
        if (result == state.pid) {
            state.last_exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
            state.has_exit_code = true;
        }
        state.pid = -1;
        return false;
    }
    return true;
#endif
}

void start_process(process_state & state, const json & input) {
    std::lock_guard<std::mutex> lock(state.mutex);
    if (process_running_locked(state)) {
        throw std::runtime_error("the inference server is already running");
    }
    const server_config config = config_from_json(input, state.config);
    validate_config(config);
    state.config = config;
    state.last_error.clear();
    state.has_exit_code = false;
    std::vector<std::string> args = {
        "--model", config.model,
        "--host", "127.0.0.1",
        "--port", std::to_string(config.port),
        "--parallel", std::to_string(config.parallel),
        "--ctx-size", std::to_string(config.ctx_size),
        "--batch-size", std::to_string(config.batch_size),
        "--ubatch-size", std::to_string(config.ubatch_size),
        "--ctx-checkpoints", std::to_string(config.ctx_checkpoints),
        "--cache-ram", std::to_string(config.cache_ram_mib),
        "--cache-type-k", config.cache_type_k,
        "--cache-type-v", config.cache_type_v,
        config.context_shift ? "--context-shift" : "--no-context-shift",
        "--moe-paging", config.mode,
        "--moe-io-threads", std::to_string(config.threads),
        "--moe-ple-cache-mib", std::to_string(config.ple_cache_mib),
        "--moe-io-mode", config.io_mode,
        "--moe-cache-policy", config.cache_policy,
        "--moe-overlap", config.overlap,
        "--moe-prefetch", config.prefetch,
        "--moe-paged-layers", std::to_string(config.paged_layers),
        "--moe-dense-residency", config.mode == "off" && config.dense_residency == "auto" ? "resident" : config.dense_residency,
        "--metrics",
        "--log-colors", "off",
        "--log-file", state.log_path, "--log-timestamps",
    };
    if (config.cache_policy == "hybrid" || config.overlap == "on" || config.prefetch != "off") {
        args.insert(args.end(), {"--n-gpu-layers", "0"});
    }
    if (config.cache > 0) {
        args.insert(args.end(), {"--moe-cache-mib", std::to_string(config.cache)});
    }
    if (config.slots > 0) {
        args.insert(args.end(), {"--moe-slots", std::to_string(config.slots)});
    }
    if (config.inference_threads > 0) {
        args.insert(args.end(), {
            "--threads", std::to_string(config.inference_threads),
            "--threads-batch", std::to_string(config.inference_threads),
        });
    }
    if (config.cpu_repack == "on") {
        args.push_back("--repack");
    } else if (config.cpu_repack == "off") {
        args.push_back("--no-repack");
    }
    if (config.mtp_enabled) {
        if (!config.mtp_model.empty()) {
            args.insert(args.end(), {"--model-draft", config.mtp_model});
        }
        args.insert(args.end(), {
            "--spec-type", "draft-mtp",
            "--moe-draft-cache-mib", std::to_string(config.mtp_cache_mib),
            "--spec-draft-n-max", std::to_string(config.mtp_draft_tokens),
            "--spec-draft-n-min", std::to_string(config.mtp_min_tokens),
            "--spec-draft-p-min", std::to_string(config.mtp_p_min),
            config.mtp_backend_sampling ? "--spec-draft-backend-sampling" : "--no-spec-draft-backend-sampling",
        });
        if (config.mtp_threads > 0) {
            args.insert(args.end(), {"--spec-draft-threads", std::to_string(config.mtp_threads)});
        }
    }

#if defined(_WIN32)
    const std::wstring executable = executable_directory() + L"\\llama-server.exe";
    if (GetFileAttributesW(executable.c_str()) == INVALID_FILE_ATTRIBUTES) {
        throw std::runtime_error("llama-server.exe is not next to SparkMoE.exe");
    }
    std::wstring command = quote_windows_arg(executable);
    for (const auto & arg : args) {
        command += L" " + quote_windows_arg(utf8_to_wide(arg));
    }
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), mutable_command.data(), nullptr, nullptr, FALSE,
            CREATE_NEW_PROCESS_GROUP, nullptr, executable_directory().c_str(), &startup, &process)) {
        throw std::runtime_error("CreateProcessW failed with code " + std::to_string(GetLastError()));
    }
    CloseHandle(process.hThread);
    state.process = process.hProcess;
    state.pid = process.dwProcessId;
#else
    const pid_t child = fork();
    if (child < 0) {
        throw std::runtime_error("fork failed");
    }
    if (child == 0) {
        std::vector<char *> argv;
        argv.push_back(const_cast<char *>("llama-server"));
        for (auto & arg : args) {
            argv.push_back(arg.data());
        }
        argv.push_back(nullptr);
        execvp("llama-server", argv.data());
        _exit(127);
    }
    state.pid = child;
#endif
    state.started_at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

void stop_process(process_state & state) {
    std::lock_guard<std::mutex> lock(state.mutex);
    if (!process_running_locked(state)) {
        return;
    }
#if defined(_WIN32)
    TerminateProcess(state.process, 0);
    WaitForSingleObject(state.process, 5000);
    DWORD code = 0;
    if (GetExitCodeProcess(state.process, &code)) {
        state.last_exit_code = static_cast<int>(code);
        state.has_exit_code = true;
    }
    CloseHandle(state.process);
    state.process = nullptr;
    state.pid = 0;
#else
    kill(state.pid, SIGTERM);
    int status = 0;
    waitpid(state.pid, &status, 0);
    state.last_exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
    state.has_exit_code = true;
    state.pid = -1;
#endif
    state.started_at_ms = 0;
    state.has_exit_code = false;
}

json status_json(process_state & state) {
    std::lock_guard<std::mutex> lock(state.mutex);
    const bool running = process_running_locked(state);
    bool ready = false;
    if (running) {
        httplib::Client client("127.0.0.1", state.config.port);
        client.set_connection_timeout(0, 250000);
        const auto response = client.Get("/health");
        ready = response && response->status == 200;
    }
    const std::string phase = ready ? "ready" : running ? "starting" : state.has_exit_code ? "exited" : "stopped";
    return {
        {"running", running},
        {"server_ready", ready},
        {"phase", phase},
        {"port", state.config.port},
        {"started_at_ms", state.started_at_ms},
        {"last_error", state.last_error},
        {"last_exit_code", state.has_exit_code ? json(state.last_exit_code) : json(nullptr)},
        {"config", config_json(state.config)},
#if defined(_WIN32)
        {"pid", state.pid},
#else
        {"pid", state.pid > 0 ? state.pid : 0},
#endif
    };
}

json current_config_json(process_state & state) {
    std::lock_guard<std::mutex> lock(state.mutex);
    return config_json(state.config);
}

std::string tail_file(const std::string & path, size_t max_bytes) {
    std::ifstream file(std::filesystem::u8path(path), std::ios::binary | std::ios::ate);
    if (!file) {
        return {};
    }
    const std::streamoff size = static_cast<std::streamoff>(file.tellg());
    const std::streamoff start = size > static_cast<std::streamoff>(max_bytes)
        ? size - static_cast<std::streamoff>(max_bytes) : 0;
    file.seekg(start);
    std::ostringstream content;
    content << file.rdbuf();
    return content.str();
}

json upstream_json(process_state & state, const std::string & path, bool post = false, const std::string & body = "") {
    const json status = status_json(state);
    if (!status["server_ready"].get<bool>()) {
        throw std::runtime_error("inference server is not ready");
    }
    httplib::Client client("127.0.0.1", status["port"].get<int>());
    client.set_connection_timeout(0, 500000);
    const auto response = post ? client.Post(path, body, "application/json") : client.Get(path);
    if (!response) {
        throw std::runtime_error("inference server did not respond");
    }
    if (response->status < 200 || response->status >= 300) {
        throw std::runtime_error("inference server returned HTTP " + std::to_string(response->status));
    }
    auto result = json::parse(response->body);
    if (!post && path == "/sparkmoe/v1/paging") {
        client.set_read_timeout(2, 0);
        const auto metrics = client.Get("/metrics");
        if (metrics && metrics->status == 200) {
            std::istringstream input(metrics->body);
            std::string line;
            double tokens = 0, seconds = 0;
            while (std::getline(input, line)) {
                std::istringstream item(line);
                std::string key;
                double value = 0;
                if (!(item >> key >> value)) { continue; }
                if (key == "llamacpp:tokens_predicted_total") { tokens = value; }
                if (key == "llamacpp:tokens_predicted_seconds_total") { seconds = value; }
            }
            result["generation_tok_s"] = seconds > 0 ? tokens / seconds : 0;
        }
    }
    return result;
}

std::string request_origin(const httplib::Request & request) {
    return request.has_header("Origin") ? request.get_header_value("Origin") : std::string();
}

bool origin_allowed(const httplib::Request & request, process_state & state) {
    const std::string origin = request_origin(request);
    if (origin.empty()) {
        return true;
    }
    int port = 8080;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        port = state.config.port;
    }
    const std::vector<std::string> allowed = {
        "http://127.0.0.1:" + std::to_string(manager_port),
        "http://localhost:" + std::to_string(manager_port),
        "http://127.0.0.1:" + std::to_string(port),
        "http://localhost:" + std::to_string(port),
    };
    return std::find(allowed.begin(), allowed.end(), origin) != allowed.end();
}

void set_cors_headers(const httplib::Request & request, httplib::Response & response) {
    const std::string origin = request_origin(request);
    if (!origin.empty()) {
        response.set_header("Access-Control-Allow-Origin", origin);
        response.set_header("Vary", "Origin");
    }
    response.set_header("Access-Control-Allow-Headers", "Content-Type");
    response.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
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
    const uint64_t used_mib = total_mib > available_mib ? total_mib - available_mib : 0;
    return {
        {"total_ram_mib", total_mib},
        {"available_ram_mib", available_mib},
        {"used_ram_mib", used_mib},
        {"cpu_threads", cpu_threads},
        {"cache_mib", cache_mib},
        {"io_threads", std::max<uint32_t>(1, std::min<uint32_t>(4, cpu_threads / 4))},
        {"inference_threads", 0},
        {"cpu_repack", "auto"},
        {"parallel", 1},
        {"ctx_size", ctx_size},
        {"batch_size", 1024},
        {"ubatch_size", 128},
        {"cache_ram_mib", 0},
        {"reason", "Keeps RAM headroom, uses one request slot, and limits prompt batches for CPU expert paging."},
    };
}

void open_browser() {
    if (std::getenv("SPARKMOE_NO_BROWSER")) {
        return;
    }
#if defined(_WIN32)
    ShellExecuteW(nullptr, L"open", L"http://127.0.0.1:18080", nullptr, nullptr, SW_SHOWNORMAL);
#else
    const pid_t child = fork();
    if (child == 0) {
        execlp("xdg-open", "xdg-open", "http://127.0.0.1:18080", static_cast<char *>(nullptr));
        _exit(127);
    }
#endif
}

void json_response(httplib::Response & response, const json & value, int status = 200) {
    response.status = status;
    response.set_content(value.dump(), "application/json; charset=utf-8");
}

} // namespace

int main() {
    process_state state;
    const json recommendation = recommendation_json();
    state.config.cache = recommendation["cache_mib"].get<int>();
    state.config.threads = recommendation["io_threads"].get<int>();
    state.config.inference_threads = recommendation["inference_threads"].get<int>();
    state.config.cpu_repack = recommendation["cpu_repack"].get<std::string>();
    state.config.ctx_size = recommendation["ctx_size"].get<int>();
    state.config.batch_size = recommendation["batch_size"].get<int>();
    state.config.ubatch_size = recommendation["ubatch_size"].get<int>();
    state.config.parallel = recommendation["parallel"].get<int>();
    state.config.cache_ram_mib = recommendation["cache_ram_mib"].get<int>();
    state.log_path = server_log_path();
    const std::string dashboard = load_dashboard();
    const std::string brand_png = load_brand_asset("SparkMoE.png");
    const std::string brand_ico = load_brand_asset("SparkMoE.ico");
    httplib::Server server;

    auto authorize = [&](const httplib::Request & request, httplib::Response & response) {
        if (!origin_allowed(request, state)) {
            json_response(response, {{"error", "request origin is not allowed"}}, 403);
            return false;
        }
        set_cors_headers(request, response);
        return true;
    };

    server.Get("/", [&dashboard](const httplib::Request &, httplib::Response & response) {
        response.set_content(dashboard, "text/html; charset=utf-8");
    });
    server.Get("/assets/SparkMoE.png", [&brand_png](const httplib::Request &, httplib::Response & response) {
        if (brand_png.empty()) {
            response.status = 404;
            return;
        }
        response.set_content(brand_png, "image/png");
        response.set_header("Cache-Control", "public, max-age=3600");
    });
    server.Get("/assets/SparkMoE.ico", [&brand_ico](const httplib::Request &, httplib::Response & response) {
        if (brand_ico.empty()) {
            response.status = 404;
            return;
        }
        response.set_content(brand_ico, "image/x-icon");
        response.set_header("Cache-Control", "public, max-age=3600");
    });
    server.Options(R"(/api/.*)", [&](const httplib::Request & request, httplib::Response & response) {
        if (authorize(request, response)) {
            response.status = 204;
        }
    });
    server.Get("/api/status", [&](const httplib::Request & request, httplib::Response & response) {
        if (!authorize(request, response)) return;
        json_response(response, status_json(state));
    });
    server.Get("/api/config", [&](const httplib::Request & request, httplib::Response & response) {
        if (!authorize(request, response)) return;
        json_response(response, current_config_json(state));
    });
    server.Get("/api/recommendation", [&](const httplib::Request & request, httplib::Response & response) {
        if (!authorize(request, response)) return;
        json_response(response, recommendation_json());
    });
    server.Get("/api/models", [&](const httplib::Request & request, httplib::Response & response) {
        if (!authorize(request, response)) return;
        try {
            const std::string directory = request.has_param("directory") ? request.get_param_value("directory") : "";
            json_response(response, models_in_directory(directory));
        } catch (const std::exception & error) {
            json_response(response, {{"error", error.what()}}, 400);
        }
    });
    server.Post("/api/browse-model-directory", [&](const httplib::Request & request, httplib::Response & response) {
        if (!authorize(request, response)) return;
        try {
            json_response(response, browse_models_directory());
        } catch (const std::exception & error) {
            json_response(response, {{"error", error.what()}}, 500);
        }
    });
    server.Post("/api/start", [&](const httplib::Request & request, httplib::Response & response) {
        if (!authorize(request, response)) return;
        try {
            start_process(state, json::parse(request.body));
            json_response(response, {{"ok", true}, {"status", status_json(state)}});
        } catch (const std::exception & error) {
            {
                std::lock_guard<std::mutex> lock(state.mutex);
                state.last_error = error.what();
            }
            json_response(response, {{"error", error.what()}}, 400);
        }
    });
    server.Post("/api/restart", [&](const httplib::Request & request, httplib::Response & response) {
        if (!authorize(request, response)) return;
        try {
            const json input = json::parse(request.body);
            server_config base;
            {
                std::lock_guard<std::mutex> lock(state.mutex);
                base = state.config;
            }
            validate_config(config_from_json(input, base));
            stop_process(state);
            start_process(state, input);
            json_response(response, {{"ok", true}, {"status", status_json(state)}});
        } catch (const std::exception & error) {
            {
                std::lock_guard<std::mutex> lock(state.mutex);
                state.last_error = error.what();
            }
            json_response(response, {{"error", error.what()}}, 400);
        }
    });
    server.Post("/api/stop", [&](const httplib::Request & request, httplib::Response & response) {
        if (!authorize(request, response)) return;
        stop_process(state);
        json_response(response, {{"ok", true}});
    });
    server.Get("/api/paging", [&](const httplib::Request & request, httplib::Response & response) {
        if (!authorize(request, response)) return;
        try {
            json_response(response, upstream_json(state, "/sparkmoe/v1/paging"));
        } catch (const std::exception & error) {
            json_response(response, {{"error", error.what()}}, 503);
        }
    });
    server.Post("/api/paging/cache", [&](const httplib::Request & request, httplib::Response & response) {
        if (!authorize(request, response)) return;
        try {
            const auto body = json::parse(request.body);
            json_response(response, upstream_json(state, "/sparkmoe/v1/paging/cache", true, body.dump()), 202);
        } catch (const std::exception & error) {
            json_response(response, {{"error", error.what()}}, 400);
        }
    });
    server.Post("/api/paging/reset", [&](const httplib::Request & request, httplib::Response & response) {
        if (!authorize(request, response)) return;
        try {
            json_response(response, upstream_json(state, "/sparkmoe/v1/paging/reset-stats", true));
        } catch (const std::exception & error) {
            json_response(response, {{"error", error.what()}}, 503);
        }
    });
    server.Get("/api/server-config", [&](const httplib::Request & request, httplib::Response & response) {
        if (!authorize(request, response)) return;
        try {
            json_response(response, upstream_json(state, "/sparkmoe/v1/config"));
        } catch (const std::exception & error) {
            json_response(response, {{"error", error.what()}}, 503);
        }
    });
    server.Get("/api/logs", [&](const httplib::Request & request, httplib::Response & response) {
        if (!authorize(request, response)) return;
        json_response(response, {{"log", tail_file(state.log_path, 65536)}});
    });
    server.Get("/api/diagnostics", [&](const httplib::Request & request, httplib::Response & response) {
        if (!authorize(request, response)) return;
        json manager_status = status_json(state);
        manager_status["config"].erase("model");
        manager_status["config"].erase("mtp_model");
        manager_status["config"].erase("model_directory");
        manager_status.erase("last_error");
        json safe_config = current_config_json(state);
        safe_config.erase("model");
        safe_config.erase("mtp_model");
        safe_config.erase("model_directory");
        json diagnostic = {
            {"sparkmoe_version", SPARKMOE_VERSION},
            {"manager", manager_status},
            {"configuration", safe_config},
#if defined(_WIN32)
            {"platform", "windows"},
#else
            {"platform", "posix"},
#endif
            {"privacy", "No prompts, generated text, API keys, tokens, usernames, or model contents are collected."},
        };
        response.set_header("Content-Disposition", "attachment; filename=sparkmoe-diagnostics.json");
        json_response(response, diagnostic);
    });
    server.set_exception_handler([](const auto &, auto & response, std::exception_ptr error) {
        try {
            if (error) {
                std::rethrow_exception(error);
            }
        } catch (const std::exception & exception) {
            json_response(response, {{"error", exception.what()}}, 500);
        }
    });
    open_browser();
    std::printf("SparkMoE manager: http://127.0.0.1:%d\n", manager_port);
    const bool ok = server.listen("127.0.0.1", manager_port);
    stop_process(state);
    return ok ? 0 : 1;
}
