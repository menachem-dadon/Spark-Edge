// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright (c) 2026 SparkMoE contributors.

#include "connections.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <stdexcept>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace sparkmoe {
namespace {
using json = nlohmann::ordered_json;
namespace fs = std::filesystem;
std::mutex connections_mutex;

std::string path_text(const fs::path & path) { return path.u8string(); }
fs::path path_native(const std::string & path) { return fs::u8path(path); }

std::string env(const char * name) {
#if defined(_WIN32)
    const auto key = path_native(name).wstring();
    const DWORD size = GetEnvironmentVariableW(key.c_str(), nullptr, 0);
    if (!size) { return {}; }
    std::wstring value(size, L'\0');
    const DWORD count = GetEnvironmentVariableW(key.c_str(), value.data(), size);
    value.resize(count);
    return path_text(fs::path(value));
#else
    const char * value = std::getenv(name);
    return value ? value : "";
#endif
}

bool safe_id(const std::string & value) {
    return !value.empty() && value.size() <= 128 && std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
}

bool safe_model_id(const std::string & value) {
    if (value.empty() || value.size() > 128 || value.front() == '.') { return false; }
    auto normalized = value;
    std::replace(normalized.begin(), normalized.end(), '.', '-');
    return safe_id(normalized);
}

int64_t context_length(const json & model) {
    const auto found = model.find("context_length");
    return found != model.end() && found->is_number_integer() ? std::max<int64_t>(0, found->get<int64_t>()) : 0;
}

void check_text(const std::string & value, const char * name, size_t max_size = 4096) {
    if (value.size() > max_size || value.find_first_of(std::string("\0\r\n", 3)) != std::string::npos) {
        throw std::runtime_error(std::string("Invalid ") + name);
    }
}

void no_links(const fs::path & path) {
    fs::path current;
    for (const auto & part : fs::absolute(path).lexically_normal()) {
        current /= part;
#if defined(_WIN32)
        const DWORD attributes = GetFileAttributesW(current.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            throw std::runtime_error("Connection storage cannot contain a junction or symbolic link");
        }
#else
        std::error_code error;
        if (fs::is_symlink(fs::symlink_status(current, error))) {
            throw std::runtime_error("Connection storage cannot contain a symbolic link");
        }
#endif
    }
}

fs::path storage_root(const connection_context & context) {
    if (context.data_directory.empty()) { throw std::runtime_error("Connection data directory is missing"); }
    const auto root = fs::absolute(context.data_directory / "connections").lexically_normal();
    no_links(root);
    return root;
}

void write_file(const fs::path & path, const std::string & contents) {
    no_links(path);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    out.close();
    if (!out) { throw std::runtime_error("Cannot write connection file: " + path_text(path)); }
}

json read_json(const fs::path & path) {
    no_links(path);
    if (!fs::is_regular_file(path) || fs::file_size(path) > 1024 * 1024) {
        throw std::runtime_error("Connection record is missing or too large");
    }
    std::ifstream input(path, std::ios::binary);
    return json::parse(input);
}

std::string unique_suffix() {
    std::random_device source;
    std::ostringstream result;
    result << std::hex << std::chrono::system_clock::now().time_since_epoch().count() << '-' << source();
    return result.str();
}

fs::path find_program(const std::string & name) {
    std::vector<fs::path> directories;
    std::stringstream paths(env("PATH"));
    std::string item;
#if defined(_WIN32)
    constexpr char separator = ';';
#else
    constexpr char separator = ':';
#endif
    while (std::getline(paths, item, separator)) {
        if (item.size() >= 2 && item.front() == '"' && item.back() == '"') { item = item.substr(1, item.size() - 2); }
        const auto path = path_native(item);
        if (path.is_absolute()) { directories.push_back(path); }
    }
#if defined(_WIN32)
    const auto home = path_native(env("USERPROFILE"));
    if (!home.empty()) {
        directories.push_back(home / ".local" / "bin");
        directories.push_back(home / ".opencode" / "bin");
    }
    const std::vector<std::string> suffixes = {".exe", ".cmd"};
#else
    const std::vector<std::string> suffixes = {""};
#endif
    for (const auto & suffix : suffixes) {
        for (const auto & directory : directories) {
            const auto candidate = directory / (name + suffix);
            std::error_code error;
            if (fs::is_regular_file(candidate, error)) {
#if !defined(_WIN32)
                if (access(candidate.c_str(), X_OK) != 0) { continue; }
#endif
                return candidate;
            }
        }
    }
    return {};
}

#if defined(_WIN32)
std::wstring quote_argument(const std::wstring & value) {
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (wchar_t c : value) {
        if (c == L'\\') { ++slashes; continue; }
        result.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        result += c;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}

std::wstring command_line(const fs::path & executable, const std::vector<std::string> & arguments) {
    std::wstring result = quote_argument(executable.wstring());
    for (const auto & argument : arguments) { result += L' ' + quote_argument(path_native(argument).wstring()); }
    return result;
}

std::string program_version(const fs::path & executable) {
    if (executable.empty() || executable.extension() != ".exe") { return {}; }
    static std::mutex version_mutex;
    static std::map<fs::path, std::pair<fs::file_time_type, std::string>> cache;
    std::lock_guard<std::mutex> lock(version_mutex);
    const auto modified = fs::last_write_time(executable);
    const auto cached = cache.find(executable);
    if (cached != cache.end() && cached->second.first == modified) { return cached->second.second; }
    SECURITY_ATTRIBUTES security = {sizeof(security), nullptr, TRUE};
    HANDLE read_pipe = nullptr, write_pipe = nullptr;
    if (!CreatePipe(&read_pipe, &write_pipe, &security, 0)) { return {}; }
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = write_pipe;
    startup.hStdError = write_pipe;
    startup.hStdInput = nullptr;
    PROCESS_INFORMATION process{};
    auto command = command_line(executable, {"--version"});
    const BOOL created = CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, executable.parent_path().c_str(), &startup, &process);
    CloseHandle(write_pipe);
    std::string version;
    if (created) {
        if (WaitForSingleObject(process.hProcess, 2000) == WAIT_TIMEOUT) { TerminateProcess(process.hProcess, 1); }
        DWORD available = 0;
        if (PeekNamedPipe(read_pipe, nullptr, 0, nullptr, &available, nullptr) && available) {
            char buffer[512];
            DWORD count = 0;
            if (ReadFile(read_pipe, buffer, std::min<DWORD>(available, sizeof(buffer)), &count, nullptr)) { version.assign(buffer, count); }
        }
        CloseHandle(process.hProcess);
        CloseHandle(process.hThread);
    }
    CloseHandle(read_pipe);
    const auto newline = version.find_first_of("\r\n");
    if (newline != std::string::npos) { version.resize(newline); }
    cache[executable] = {modified, version};
    return version;
}
#else
std::string program_version(const fs::path &) { return {}; }
#endif

json connector_info(const std::string & id) {
    std::string name, binary, support;
    if (id == "codex") { name = "Codex CLI"; binary = "codex"; support = "supported"; }
    else if (id == "opencode") { name = "OpenCode"; binary = "opencode"; support = "supported"; }
    else if (id == "continue") { name = "Continue"; binary = "cn"; support = "export"; }
    else if (id == "claude-code") { name = "Claude Code"; binary = "claude"; support = "experimental"; }
    else if (id == "codex-desktop") {
        return {{"id", id}, {"name", "Codex Desktop"}, {"installed", false}, {"executable", ""}, {"version", ""},
            {"support", "manual"}, {"launch_supported", false},
            {"description", "Custom model availability depends on the installed desktop version. Use the isolated Codex CLI connection; Spark Edge does not change the running desktop app or its model picker."}};
    } else { throw std::runtime_error("Unknown connector"); }
    auto executable = find_program(binary);
    bool version_two = false;
    if (id == "opencode" && executable.empty()) { executable = find_program("opencode2"); version_two = !executable.empty(); }
    const auto version = program_version(executable);
    if (id == "opencode" && (version.rfind("2.", 0) == 0 || version.find(" 2.") != std::string::npos)) { version_two = true; }
#if defined(_WIN32)
    bool launch = !executable.empty() && executable.extension() == ".exe";
#else
    bool launch = false;
#endif
    launch = launch && id != "continue" && !version_two;
    return {{"id", id}, {"name", name}, {"installed", !executable.empty()}, {"executable", path_text(executable)},
        {"version", version}, {"support", version_two ? "export" : support}, {"launch_supported", launch}, {"version_two", version_two},
        {"description", id == "claude-code" ? "Experimental: a custom local model using the Anthropic-compatible API. Non-Claude models are not supported by Anthropic." :
            id == "continue" ? "Export a separate YAML configuration for the Continue model selector." :
            version_two ? "OpenCode 2 configuration export. Automatic isolated launch is not verified for this version." :
            "Launch with process-local provider settings. Existing account and permission settings remain in place."}};
}

json selected_models(const json & input, const connection_context & context) {
    const auto id = input.value("model_id", "");
    if (!safe_model_id(id)) { throw std::runtime_error("Select a valid model ID"); }
    json models = json::array();
    for (const auto & model : context.models) {
        if (model.value("id", "") == id) { models.push_back(model); break; }
    }
    if (models.empty()) { throw std::runtime_error("The selected model is no longer available"); }
    if (input.value("include_favorites", false)) {
        for (const auto & model : context.models) {
            if (model.value("favorite", false) && model.value("id", "") != id && safe_model_id(model.value("id", ""))) { models.push_back(model); }
        }
    }
    return models;
}

json make_preview(const json & input, const connection_context & context) {
    const auto connector = input.value("connector", "");
    const auto info = connector_info(connector);
    if (connector == "codex-desktop") { throw std::runtime_error("Use Codex CLI for an isolated custom-provider connection"); }
    const auto models = selected_models(input, context);
    const auto id = models[0].at("id").get<std::string>();
    const auto project = input.value("project_dir", "");
    check_text(project, "project directory");
    check_text(context.api_base_url, "API URL");
    if (context.api_base_url.rfind("http://127.0.0.1:", 0) != 0 || context.api_base_url.size() < 4 ||
        context.api_base_url.substr(context.api_base_url.size() - 3) != "/v1") {
        throw std::runtime_error("Connections require the local Spark Edge API endpoint");
    }
    const auto port = context.api_base_url.substr(17, context.api_base_url.size() - 20);
    if (port.empty() || port.size() > 5 || !std::all_of(port.begin(), port.end(), [](char c) { return c >= '0' && c <= '9'; }) ||
        std::stoi(port) < 1 || std::stoi(port) > 65535) { throw std::runtime_error("Invalid local API port"); }
    if (!project.empty() && (!path_native(project).is_absolute() || !fs::is_directory(path_native(project)))) {
        throw std::runtime_error("Select an existing absolute project directory");
    }
    json files = json::array(), arguments = json::array(), environment = json::object(), warnings = json::array();
    auto add_file = [&](const std::string & name, const std::string & content) { files.push_back({{"name", name}, {"content", content}}); };
    if (connector == "codex") {
        const std::string provider = "{name=\"Spark Edge\",base_url=" + json(context.api_base_url).dump() +
            ",env_key=\"SPARKMOE_API_KEY\",requires_openai_auth=false,wire_api=\"responses\"}";
        arguments = {"-c", "model_provider=\"sparkmoe\"", "-c", "model_providers.sparkmoe=" + provider, "--model", id};
        if (context_length(models[0]) > 0) {
            arguments.push_back("-c");
            arguments.push_back("model_context_window=" + std::to_string(context_length(models[0])));
        }
        if (!project.empty()) { arguments.push_back("--cd"); arguments.push_back(project); }
        add_file("provider.toml", "model = " + json(id).dump() + "\nmodel_provider = \"sparkmoe\"\n\n[model_providers.sparkmoe]\nname = \"Spark Edge\"\nbase_url = " +
            json(context.api_base_url).dump() + "\nenv_key = \"SPARKMOE_API_KEY\"\nrequires_openai_auth = false\nwire_api = \"responses\"\n");
        warnings.push_back("Codex CLI requires the Responses API. The selected model uses its own ID; the desktop model picker is unchanged.");
        if (models.size() > 1) { warnings.push_back("Each favourite is exported in models.json. Select another model in Spark Edge to create its Codex launcher."); }
    } else if (connector == "opencode") {
        json entries = json::object();
        for (const auto & model : models) {
            json entry = {{"name", model.value("display_name", model.at("id").get<std::string>())}};
            const auto window = context_length(model);
            if (window > 0) { entry["limit"] = {{"context", window}, {"output", std::max<int64_t>(1, std::min<int64_t>(8192, window / 4))}}; }
            entries[model.at("id").get<std::string>()] = entry;
        }
        json config = {{"$schema", "https://opencode.ai/config.json"}, {"model", "sparkmoe/" + id}};
        if (info.value("version_two", false)) {
            config["providers"]["sparkmoe"] = {
                {"name", "Spark Edge"}, {"env", {"SPARKMOE_API_KEY"}}, {"package", "@opencode/ai/providers/openai-compatible"},
                {"settings", {{"baseURL", context.api_base_url}}}, {"models", entries}};
        } else {
            config["small_model"] = "sparkmoe/" + id;
            config["provider"]["sparkmoe"] = {
                {"npm", "@ai-sdk/openai-compatible"}, {"name", "Spark Edge"}, {"options", {{"baseURL", context.api_base_url}, {"apiKey", "{env:SPARKMOE_API_KEY}"}}},
                {"models", entries}};
            environment["OPENCODE_CONFIG_CONTENT"] = config.dump();
        }
        arguments = {"--model", "sparkmoe/" + id};
        add_file("opencode.json", config.dump(2) + "\n");
    } else if (connector == "continue") {
        std::string yaml = "name: Spark Edge\nversion: 1.0.0\nschema: v1\nmodels:\n";
        for (const auto & model : models) {
            yaml += "  - name: " + json(model.value("display_name", model.at("id").get<std::string>())).dump() + "\n    provider: openai\n    model: " +
                model.at("id").dump() + "\n    apiBase: " + json(context.api_base_url).dump() +
                "\n    apiKey: ${{ secrets.SPARKMOE_API_KEY }}\n    useResponsesApi: false\n    roles: [chat, edit, apply]\n";
            if (model.contains("supports_tools") && model["supports_tools"].is_boolean()) {
                yaml += model["supports_tools"].get<bool>() ? "    capabilities: [tool_use]\n" : "    capabilities: []\n";
            }
            if (context_length(model) > 0) {
                yaml += "    defaultCompletionOptions:\n      contextLength: " + std::to_string(context_length(model)) + "\n";
            }
        }
        add_file("config.yaml", yaml);
        warnings.push_back("Import the YAML as a separate Continue configuration and supply SPARKMOE_API_KEY as a Continue secret. Existing IDE settings are not changed.");
    } else if (connector == "claude-code") {
        if (!input.value("experimental", false)) { warnings.push_back("Explicit experimental opt-in is required before configuration or launch."); }
        environment = {{"ANTHROPIC_BASE_URL", context.api_base_url.substr(0, context.api_base_url.size() - 3)},
            {"ANTHROPIC_CUSTOM_MODEL_OPTION", id}, {"ANTHROPIC_CUSTOM_MODEL_OPTION_NAME", models[0].value("display_name", id)},
            {"ANTHROPIC_CUSTOM_MODEL_OPTION_DESCRIPTION", "Experimental local Spark Edge model"},
            {"ANTHROPIC_DEFAULT_OPUS_MODEL", id}, {"ANTHROPIC_DEFAULT_SONNET_MODEL", id}, {"ANTHROPIC_DEFAULT_HAIKU_MODEL", id},
            {"CLAUDE_CODE_SUBAGENT_MODEL", id}};
        arguments = {"--model", id};
        add_file("environment.json", environment.dump(2) + "\n");
        warnings.push_back("Experimental: Anthropic does not support non-Claude models. Tool calling, compaction and model-specific behaviour need validation with the selected model.");
    }
    add_file("models.json", json{{"models", models}}.dump(2) + "\n");
    if (!info.value("installed", false) && connector != "continue") { warnings.push_back("The application is not installed or its executable is not on PATH. Spark Edge does not install external applications."); }
    if (info.value("installed", false) && !info.value("launch_supported", false) && connector != "continue") {
        warnings.push_back("Automatic launch needs a supported native Windows executable. Use the exported configuration for this installation.");
    }
    if (!models[0].contains("supports_tools") || !models[0]["supports_tools"].is_boolean()) {
        warnings.push_back("Tool-calling capability has not been verified for this model.");
    } else if (!models[0]["supports_tools"].get<bool>()) { warnings.push_back("This model does not advertise tool calling; coding-agent features may not work."); }
    return {{"connector", connector}, {"model_id", id}, {"project_dir", project}, {"files", files}, {"arguments", arguments}, {"environment", environment},
        {"warnings", warnings}, {"support", info.at("support")}, {"launch_supported", info.at("launch_supported")}, {"executable", info.at("executable")}};
}

json saved_record(const fs::path & root, const std::string & id) {
    if (!safe_id(id)) { throw std::runtime_error("Invalid connection ID"); }
    auto record = read_json(root / id / "connection.json");
    if (record.value("id", "") != id || record.value("owner", "") != "SparkMoE") { throw std::runtime_error("Not a Spark Edge connection"); }
    return record;
}

json save_connection(const json & input, const connection_context & context) {
    auto preview = make_preview(input, context);
    const auto connector = input.at("connector").get<std::string>();
    if (connector == "claude-code" && !input.value("experimental", false)) { throw std::runtime_error("Enable experimental Claude Code support explicitly"); }
    const auto root = storage_root(context);
    std::string id = input.value("id", "");
    const bool update = !id.empty();
    if (update) { saved_record(root, id); }
    else { id = connector + '-' + unique_suffix(); }
    if (!safe_id(id)) { throw std::runtime_error("Invalid connection ID"); }
    fs::create_directories(root);
    const auto destination = root / id;
    const auto staging = root / (".staging-" + unique_suffix());
    const auto backup = root / ".backups" / (id + '-' + unique_suffix());
    no_links(destination);
    no_links(staging);
    fs::create_directory(staging);
    json record = {{"owner", "SparkMoE"}, {"schema_version", 1}, {"id", id}, {"connector", connector}, {"model_id", input.at("model_id")},
        {"project_dir", input.value("project_dir", "")}, {"include_favorites", input.value("include_favorites", false)},
        {"experimental", input.value("experimental", false)}, {"config_dir", path_text(destination)}, {"api_base_url", context.api_base_url}};
    for (const auto & file : preview.at("files")) { write_file(staging / file.at("name").get<std::string>(), file.at("content").get<std::string>()); }
    write_file(staging / "connection.json", record.dump(2) + "\n");
    write_file(staging / "launch.json", json{{"executable", preview.at("executable")}, {"arguments", preview.at("arguments")},
        {"environment", preview.at("environment")}, {"credential_environment", "SPARKMOE_API_KEY"}}.dump(2) + "\n");
    if (update) {
        no_links(backup);
        fs::create_directories(backup.parent_path());
        fs::rename(destination, backup);
    }
    try { fs::rename(staging, destination); }
    catch (...) { if (update) { fs::rename(backup, destination); } throw; }
    preview["connection"] = record;
    if (update) { preview["backup_path"] = path_text(backup); }
    return preview;
}

json launch_connection(const json & record, const connection_context & context) {
    const auto preview = make_preview(record, context);
    if (!preview.at("launch_supported").get<bool>()) { throw std::runtime_error("Automatic launch is unavailable. Use the exported configuration or install the supported native application."); }
    if (record.value("connector", "") == "claude-code" && !record.value("experimental", false)) { throw std::runtime_error("Experimental connector was not enabled"); }
    const auto project = record.value("project_dir", "");
    if (project.empty()) { throw std::runtime_error("Choose a project directory before launching an agent"); }
    if (context.api_key.empty()) { throw std::runtime_error("A local Spark Edge API key is required for launch"); }
    check_text(context.api_key, "local API key");
    if (!context.ensure_model_ready) { throw std::runtime_error("Model readiness check is unavailable"); }
    context.ensure_model_ready(record.at("model_id").get<std::string>());
#if defined(_WIN32)
    const auto executable = path_native(preview.at("executable").get<std::string>());
    auto command = command_line(executable, preview.at("arguments").get<std::vector<std::string>>());
    struct case_insensitive {
        bool operator()(const std::wstring & a, const std::wstring & b) const { return _wcsicmp(a.c_str(), b.c_str()) < 0; }
    };
    std::map<std::wstring, std::wstring, case_insensitive> variables;
    wchar_t * inherited = GetEnvironmentStringsW();
    if (!inherited) { throw std::runtime_error("Cannot read process environment"); }
    for (const wchar_t * item = inherited; *item; item += wcslen(item) + 1) {
        const std::wstring value(item);
        const auto delimiter = value.find(L'=', value.front() == L'=' ? 1 : 0);
        if (delimiter != std::wstring::npos) { variables[value.substr(0, delimiter)] = value.substr(delimiter + 1); }
    }
    FreeEnvironmentStringsW(inherited);
    for (const auto & entry : preview.at("environment").items()) { variables[path_native(entry.key()).wstring()] = path_native(entry.value().get<std::string>()).wstring(); }
    variables[L"SPARKMOE_API_KEY"] = path_native(context.api_key).wstring();
    if (record.value("connector", "") == "claude-code") {
        variables[L"ANTHROPIC_AUTH_TOKEN"] = path_native(context.api_key).wstring();
        variables[L"ANTHROPIC_API_KEY"] = path_native(context.api_key).wstring();
    }
    std::vector<wchar_t> environment;
    for (const auto & variable : variables) {
        const auto line = variable.first + L'=' + variable.second;
        environment.insert(environment.end(), line.begin(), line.end());
        environment.push_back(L'\0');
    }
    environment.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE | CREATE_UNICODE_ENVIRONMENT,
        environment.data(), path_native(project).c_str(), &startup, &process)) {
        throw std::runtime_error("Cannot launch connector: Windows error " + std::to_string(GetLastError()));
    }
    const auto pid = process.dwProcessId;
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    return {{"launched", true}, {"id", record.at("id")}, {"pid", pid}, {"warnings", preview.at("warnings")}};
#else
    throw std::runtime_error("Automatic launch is available on Windows. Use the exported configuration on this platform.");
#endif
}
}

json connections_dispatch(const std::string & action, const json & input, const connection_context & context) {
    std::unique_lock<std::mutex> lock(connections_mutex);
    if (!input.is_object()) { throw std::runtime_error("Connection request must be a JSON object"); }
    if (action == "preview") { return make_preview(input, context); }
    if (action == "configure") { return save_connection(input, context); }
    const auto root = storage_root(context);
    if (action == "list") {
        json connectors = json::array(), saved = json::array(), warnings = json::array();
        for (const auto & id : {"codex", "opencode", "continue", "claude-code", "codex-desktop"}) { connectors.push_back(connector_info(id)); }
        if (fs::is_directory(root)) {
            for (const auto & entry : fs::directory_iterator(root)) {
                const auto id = path_text(entry.path().filename());
                if (!safe_id(id) || !entry.is_directory()) { continue; }
                try { saved.push_back(saved_record(root, id)); }
                catch (const std::exception &) { warnings.push_back("Skipped an invalid connection record: " + id); }
            }
        }
        return {{"connectors", connectors}, {"connections", saved}, {"warnings", warnings}, {"api_base_url", context.api_base_url}};
    }
    if (action == "launch") {
        const auto record = saved_record(root, input.value("id", ""));
        // Readiness can load a model and must not block other connection requests.
        lock.unlock();
        return launch_connection(record, context);
    }
    if (action == "remove") {
        const auto id = input.value("id", "");
        saved_record(root, id);
        const auto backup = root / ".backups" / (id + "-removed-" + unique_suffix());
        no_links(backup);
        fs::create_directories(backup.parent_path());
        fs::rename(root / id, backup);
        return {{"removed", true}, {"id", id}, {"backup_path", path_text(backup)}};
    }
    throw std::runtime_error("Unknown connection action");
}
}
