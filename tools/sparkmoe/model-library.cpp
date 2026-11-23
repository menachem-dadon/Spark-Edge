// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright (c) 2026 SparkMoE contributors.
#include "model-library.h"
#include "preset.h"
#include "gguf.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <memory>
#include <regex>
#include <stdexcept>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace sparkmoe {
void atomic_write(const std::filesystem::path & path, const std::string & content, bool backup) {
    const auto pending = std::filesystem::path(path.native() + std::filesystem::path(".pending").native());
    {
        std::ofstream output(pending, std::ios::binary | std::ios::trunc);
        if (!output || !output.write(content.data(), static_cast<std::streamsize>(content.size())).flush()) {
            throw std::runtime_error("Cannot save settings. Check free space and folder permissions.");
        }
    }
    if (backup && std::filesystem::exists(path)) {
        std::filesystem::copy_file(path, std::filesystem::path(path.native() + std::filesystem::path(".bak").native()),
            std::filesystem::copy_options::overwrite_existing);
    }
#if defined(_WIN32)
    if (!MoveFileExW(pending.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw std::runtime_error("Cannot replace the settings file.");
    }
#else
    std::filesystem::rename(pending, path);
#endif
}

std::string model_slug(const std::string & name) {
    std::string result;
    for (unsigned char c : name) {
        if (c < 128 && std::isalnum(c)) result.push_back(static_cast<char>(std::tolower(c)));
        else if ((c == '.' || c == '_' || c == '-') && !result.empty()) result.push_back(c == '_' ? '-' : c);
        else if (!result.empty() && result.back() != '-') result.push_back('-');
        if (result.size() >= 72) break;
    }
    while (!result.empty() && (result.back() == '-' || result.back() == '.')) result.pop_back();
    return result.empty() ? "model" : result;
}

library_json inspect_model(const std::filesystem::path & path) {
    library_json result = library_json::object();
    std::ifstream input(path, std::ios::binary);
    char magic[4]{};
    uint32_t version = 0;
    uint64_t tensors = 0, keys = 0;
    input.read(magic, 4).read(reinterpret_cast<char *>(&version), 4)
        .read(reinterpret_cast<char *>(&tensors), 8).read(reinterpret_cast<char *>(&keys), 8);
    if (!input || std::string(magic, 4) != "GGUF" || version < 2 || version > 3 || tensors > 1000000 || keys > 1000000) {
        throw std::runtime_error("The file does not contain a supported GGUF model.");
    }
    const gguf_init_params params{true, nullptr};
    std::unique_ptr<gguf_context, decltype(&gguf_free)> metadata(gguf_init_from_file(path.u8string().c_str(), params), gguf_free);
    if (!metadata) throw std::runtime_error("Cannot read model metadata. Check that every model file is complete.");
    auto value = [&](const std::string & key) -> library_json {
        const int64_t id = gguf_find_key(metadata.get(), key.c_str());
        if (id < 0) return nullptr;
        switch (gguf_get_kv_type(metadata.get(), id)) {
            case GGUF_TYPE_STRING: return std::string(gguf_get_val_str(metadata.get(), id));
            case GGUF_TYPE_UINT16: return gguf_get_val_u16(metadata.get(), id);
            case GGUF_TYPE_UINT8: return gguf_get_val_u8(metadata.get(), id);
            case GGUF_TYPE_UINT32: return gguf_get_val_u32(metadata.get(), id);
            case GGUF_TYPE_UINT64: return gguf_get_val_u64(metadata.get(), id);
            case GGUF_TYPE_INT32: return gguf_get_val_i32(metadata.get(), id);
            case GGUF_TYPE_FLOAT32: return gguf_get_val_f32(metadata.get(), id);
            default: return nullptr;
        }
    };
    result["architecture"] = value("general.architecture");
    result["name"] = value("general.name");
    result["file_type"] = value("general.file_type");
    const auto arch = result["architecture"].is_string() ? result["architecture"].get<std::string>() : "";
    for (const auto & entry : {"context_length", "embedding_length", "block_count", "expert_count", "attention.head_count", "attention.head_count_kv", "attention.key_length", "attention.value_length"}) {
        result[entry] = value(arch + "." + entry);
    }
    result["split_count"] = value("split.count");
    uint64_t expert_bytes = 0, resident_bytes = 0;
    const auto file_size = std::filesystem::file_size(path);
    for (int64_t i = 0; i < gguf_get_n_tensors(metadata.get()); ++i) {
        const std::string name = gguf_get_tensor_name(metadata.get(), i);
        const uint64_t end = gguf_get_data_offset(metadata.get()) + gguf_get_tensor_offset(metadata.get(), i) + gguf_get_tensor_size(metadata.get(), i);
        if (end > file_size) throw std::runtime_error("The model file is incomplete. Download it again or resume the download.");
        if (name.find("_exps.") != std::string::npos) expert_bytes += gguf_get_tensor_size(metadata.get(), i);
        else resident_bytes += gguf_get_tensor_size(metadata.get(), i);
    }
    result["expert_bytes"] = expert_bytes;
    result["resident_bytes"] = resident_bytes;
    result["supports_reasoning"] = nullptr;
    result["supports_tools"] = nullptr;
    result["supports_mtp"] = false;
    return result;
}

model_files model_directory_snapshot(const std::filesystem::path & path) {
    model_files files;
    if (path.empty()) return files;
    std::error_code error;
    std::filesystem::recursive_directory_iterator entry(path, std::filesystem::directory_options::skip_permission_denied, error);
    const std::filesystem::recursive_directory_iterator end;
    while (entry != end) {
        auto extension = entry->path().extension().u8string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (extension == ".gguf" && entry->is_regular_file(error) && !error) {
            const auto modified = entry->last_write_time(error);
            if (!error) {
                const auto size = entry->file_size(error);
                if (!error) files.emplace(entry->path(), std::make_pair(modified, size));
            }
        }
        error.clear();
        entry.increment(error);
    }
    return files;
}

bool is_draft_model(const library_json & model) {
    const auto metadata = model.value("metadata", library_json::object());
    const auto architecture = metadata.is_object() ? metadata.value("architecture", library_json(nullptr)) : library_json(nullptr);
    if (architecture == "dflash" || architecture == "dspark" || architecture == "lilicorr" || architecture == "eagle3") return true;

    const auto name = std::filesystem::u8path(model.value("path", std::string{})).stem().u8string();
    static const std::regex draft_name(
        R"((^|[-_. ])(dspark|dflash2?|lilicorr|eagle3)([-_. ]|$)|^mtp[-_. ]|[-_. ]mtp$)",
        std::regex::icase);
    return std::regex_search(name, draft_name);
}

library_json discover_models(const std::filesystem::path & source) {
    const auto path = std::filesystem::weakly_canonical(source);
    const bool single = std::filesystem::is_regular_file(path);
    if (!single && !std::filesystem::is_directory(path)) throw std::runtime_error("Select a readable GGUF file or model folder.");
    common_preset_context context(LLAMA_EXAMPLE_SERVER);
    const auto presets = context.load_from_models_dir((single ? path.parent_path() : path).u8string());
    library_json result = library_json::array();
    for (const auto & item : presets) {
        std::string model, projector, draft;
        item.second.get_option("LLAMA_ARG_MODEL", model);
        item.second.get_option("LLAMA_ARG_MMPROJ", projector);
        item.second.get_option("LLAMA_ARG_SPEC_DRAFT_MODEL", draft);
        const auto model_path = std::filesystem::weakly_canonical(std::filesystem::u8path(model));
        if (single && model_path != path) continue;
        library_json entry = {{"path", model_path.u8string()}, {"legacy_ids", {item.first}},
            {"display_name", model_path.stem().u8string()}, {"favorite", false}, {"available", true},
            {"size_bytes", std::filesystem::file_size(model_path)}, {"mmproj", projector}, {"mtp_model", draft}};
        const std::string stem = model_path.stem().u8string();
        const auto shard = stem.rfind("-00001-of-");
        if (shard != std::string::npos) entry["display_name"] = stem.substr(0, shard);
        entry["id"] = model_slug(entry["display_name"].get<std::string>());
        try {
            auto metadata = inspect_model(model_path);
            entry["files"] = library_json::array({model_path.u8string()});
            const auto split = metadata.value("split_count", library_json(nullptr));
            if (split.is_number_integer() && split.get<int>() > 1) {
                const int count = split.get<int>();
                if (count > 99999 || shard == std::string::npos) throw std::runtime_error("The split model filename is invalid.");
                uint64_t bytes = entry["size_bytes"].get<uint64_t>();
                uint64_t resident = metadata["resident_bytes"].get<uint64_t>();
                uint64_t expert = metadata["expert_bytes"].get<uint64_t>();
                for (int part = 2; part <= count; ++part) {
                    char suffix[32];
                    std::snprintf(suffix, sizeof(suffix), "-%05d-of-%05d.gguf", part, count);
                    const auto file = model_path.parent_path() / std::filesystem::u8path(stem.substr(0, shard) + suffix);
                    if (!std::filesystem::is_regular_file(file)) throw std::runtime_error("A model part is missing. Add all GGUF parts to the same folder.");
                    const auto part_metadata = inspect_model(file);
                    resident += part_metadata["resident_bytes"].get<uint64_t>();
                    expert += part_metadata["expert_bytes"].get<uint64_t>();
                    bytes += std::filesystem::file_size(file);
                    entry["files"].push_back(file.u8string());
                }
                metadata["resident_bytes"] = resident;
                metadata["expert_bytes"] = expert;
                entry["size_bytes"] = bytes;
            }
            if (!projector.empty()) {
                metadata["resident_bytes"] = metadata["resident_bytes"].get<uint64_t>() + std::filesystem::file_size(std::filesystem::u8path(projector));
            }
            entry["metadata"] = std::move(metadata);
        }
        catch (const std::exception & error) { entry["metadata"] = library_json::object(); entry["error"] = error.what(); entry["available"] = false; }
        result.push_back(std::move(entry));
    }
    if (single && result.empty()) throw std::runtime_error("Select the first GGUF part, or select the folder containing all parts.");
    return result;
}
}
