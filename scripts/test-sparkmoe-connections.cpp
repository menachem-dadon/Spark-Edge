// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright (c) 2026 SparkMoE contributors.

// Standalone harness: compile this translation unit with vendor/ on the include path.
#include "../tools/sparkmoe/connections.cpp"

#include <iostream>
#if defined(_WIN32)
#include <shellapi.h>
#endif

namespace {
using json = nlohmann::ordered_json;
namespace fs = std::filesystem;

void require(bool result, const char * message) {
    if (!result) { throw std::runtime_error(message); }
}

template <typename Function> void rejects(Function function, const char * message) {
    try { function(); }
    catch (const std::exception &) { return; }
    throw std::runtime_error(message);
}

std::string contents(const json & preview, const std::string & name) {
    for (const auto & file : preview.at("files")) {
        if (file.at("name") == name) { return file.at("content").get<std::string>(); }
    }
    throw std::runtime_error("Expected preview file is missing");
}
}

int main(int argc, char ** argv) {
    try {
        const auto root = argc > 1 ? fs::absolute(fs::u8path(argv[1])) : fs::temp_directory_path() / ("sparkmoe-connections-" + sparkmoe::unique_suffix());
        fs::create_directories(root / "project & symbols $()");
        sparkmoe::connection_context context;
        context.data_directory = root;
        context.api_key = "test-local-key-never-written";
        context.models = {
            {{"id", "qwen3.5-coder"}, {"display_name", "Qwen \"Coder\"\nmodel_provider = evil"}, {"favorite", true}, {"context_length", 32768}, {"supports_tools", true}},
            {{"id", "other-model"}, {"display_name", "Other"}, {"favorite", true}, {"context_length", nullptr}, {"supports_tools", nullptr}},
            {{"id", "not-favorite"}, {"favorite", false}}
        };
        auto call = [&](const std::string & action, const json & body) { return sparkmoe::connections_dispatch(action, body, context); };
        json body = {{"connector", "codex"}, {"model_id", "qwen3.5-coder"}, {"project_dir", (root / "project & symbols $()").u8string()}};
        const auto codex = call("preview", body);
        require(!fs::exists(root / "connections"), "Preview wrote persistent connection files");
        const auto toml = contents(codex, "provider.toml");
        require(toml.find("wire_api = \"responses\"") != std::string::npos, "Codex wire API is incorrect");
        require(toml.find("requires_openai_auth = false") != std::string::npos, "Codex may inherit account authentication");
        require(toml.find(context.api_key) == std::string::npos, "Provider export contains a secret");
        require(codex.at("arguments").back() == body.at("project_dir"), "Project directory arguments were modified");
        require(codex.dump().find("dangerously") == std::string::npos, "Preview weakens agent permissions");
        require(json::parse(contents(codex, "models.json")).at("models").size() == 1, "Default export includes unrelated models");

        body["include_favorites"] = true;
        body["connector"] = "opencode";
        auto preview = call("preview", body);
        const auto opencode = json::parse(contents(preview, "opencode.json"));
        const auto providers = opencode.contains("provider") ? opencode.at("provider") : opencode.at("providers");
        require(providers.at("sparkmoe").at("models").size() == 2, "Favourite export is incorrect");
        require(providers.at("sparkmoe").at("models").at("qwen3.5-coder").at("limit").at("context") == 32768, "Known context is missing");
        require(!providers.at("sparkmoe").at("models").at("other-model").contains("limit"), "Unknown context was invented");
        require(opencode.dump().find(context.api_key) == std::string::npos, "OpenCode export contains a secret");

        body["connector"] = "continue";
        preview = call("preview", body);
        const auto yaml = contents(preview, "config.yaml");
        require(yaml.find("capabilities: [tool_use]") != std::string::npos, "Continue tool support is missing");
        require(yaml.find("${{ secrets.SPARKMOE_API_KEY }}") != std::string::npos, "Continue must use a secret reference");
        require(!preview.at("launch_supported").get<bool>(), "Continue export falsely claims launch support");
        body["connector"] = "claude-code";
        rejects([&] { call("configure", body); }, "Claude Code configured without experimental opt-in");
        body["experimental"] = true;
        preview = call("preview", body);
        require(preview.at("environment").at("ANTHROPIC_CUSTOM_MODEL_OPTION") == body.at("model_id"), "Claude connector spoofs a model identity");
        require(preview.at("environment").at("ANTHROPIC_BASE_URL") == "http://127.0.0.1:18080", "Anthropic base URL is incorrect");

        body["connector"] = "continue";
        const auto saved = call("configure", body);
        const auto id = saved.at("connection").at("id").get<std::string>();
        require(fs::is_regular_file(root / "connections" / id / "config.yaml"), "Connection was not saved");
        body["id"] = id;
        body["include_favorites"] = false;
        const auto updated = call("configure", body);
        require(fs::is_regular_file(fs::u8path(updated.at("backup_path").get<std::string>()) / "config.yaml"), "Update backup is missing");
        require(call("list", json::object()).at("connections").size() == 1, "Connection list is incorrect");
        const auto removed = call("remove", {{"id", id}});
        require(!fs::exists(root / "connections" / id), "Removed connection is still active");
        require(fs::is_regular_file(fs::u8path(removed.at("backup_path").get<std::string>()) / "connection.json"), "Removed connection cannot be restored");
        rejects([&] { call("remove", {{"id", "../outside"}}); }, "Path traversal was accepted");
        body.erase("id");
        body["model_id"] = "..";
        rejects([&] { call("preview", body); }, "Invalid model ID was accepted");
        body["model_id"] = "missing";
        rejects([&] { call("preview", body); }, "Missing model was accepted");
        body["model_id"] = "qwen3.5-coder";
        body["project_dir"] = "relative/path";
        rejects([&] { call("preview", body); }, "Relative project directory was accepted");
        body["project_dir"] = std::string("bad\npath");
        rejects([&] { call("preview", body); }, "Control characters in project directory were accepted");
        rejects([&] { call("preview", json::array()); }, "Non-object request was accepted");

#if defined(_WIN32)
        const std::vector<std::wstring> arguments = {L"", L"a b", L"quote\"value", L"C:\\folder with space\\", L"amp & percent % dollar $()", L"slashes\\\\\"quote"};
        std::wstring command = L"test.exe";
        for (const auto & argument : arguments) { command += L' ' + sparkmoe::quote_argument(argument); }
        int count = 0;
        wchar_t ** parsed = CommandLineToArgvW(command.c_str(), &count);
        require(parsed && count == static_cast<int>(arguments.size()) + 1, "Windows argv count mismatch");
        for (size_t i = 0; i < arguments.size(); ++i) { require(parsed[i + 1] == arguments[i], "Windows argument quoting changed a value"); }
        LocalFree(parsed);
#endif
        for (const auto & entry : fs::recursive_directory_iterator(root / "connections")) {
            if (!entry.is_regular_file()) { continue; }
            std::ifstream file(entry.path(), std::ios::binary);
            const std::string text((std::istreambuf_iterator<char>(file)), {});
            require(text.find(context.api_key) == std::string::npos, "Saved connection contains a secret");
        }
        std::cout << "Connections config, escaping, backup, removal, validation and secret isolation checks passed.\nFixtures: " << root.u8string() << '\n';
        return 0;
    } catch (const std::exception & error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
