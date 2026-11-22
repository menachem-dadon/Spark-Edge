// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright (c) 2026 SparkMoE contributors.

#include "engine-packages.h"
#include "downloads.h"
#include "model-library.h"

extern "C" {
#include "hash/sha256/sha256.h"
}

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <regex>
#include <stdexcept>
#include <unordered_set>
#include <vector>

namespace sparkmoe {
using json = nlohmann::ordered_json;
namespace {

constexpr uint64_t manifest_limit = 256 * 1024;
constexpr uint64_t binary_limit = uint64_t(2) * 1024 * 1024 * 1024;
constexpr const char * release_api = "https://api.github.com/repos/menachem-dadon/Spark-Edge/releases?per_page=25";
constexpr const char * asset_url = "https://github.com/menachem-dadon/Spark-Edge/releases/download/";

bool valid_sha256(const std::string & value) {
    static const std::regex pattern("[0-9a-f]{64}");
    return std::regex_match(value, pattern);
}

bool valid_id(const std::string & value) {
    static const std::regex pattern("(cpu|cuda|vulkan|hip|sycl)-[0-9a-f]{40}");
    return std::regex_match(value, pattern);
}

bool binary_name(const std::string & value) {
    static const std::regex pattern(R"([A-Za-z0-9][A-Za-z0-9_-]{0,79}\.dll)");
    return value == "llama-server.exe" || std::regex_match(value, pattern);
}

std::string hex_digest(sha256_t & state) {
    std::array<unsigned char, SHA256_DIGEST_SIZE> digest{};
    sha256_final(&state, digest.data());
    std::string hex;
    constexpr char digits[] = "0123456789abcdef";
    for (const auto byte : digest) {
        hex.push_back(digits[byte >> 4]);
        hex.push_back(digits[byte & 15]);
    }
    return hex;
}

std::string file_digest(const std::filesystem::path & path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("An engine file could not be read.");
    sha256_t state;
    sha256_init(&state);
    std::array<char, 65536> data;
    while (stream.read(data.data(), data.size()) || stream.gcount()) {
        sha256_update(&state, reinterpret_cast<const unsigned char *>(data.data()), stream.gcount());
    }
    if (!stream.eof()) throw std::runtime_error("Reading an engine file failed.");
    return hex_digest(state);
}

struct asset {
    std::string name;
    std::string url;
    std::string sha256;
    uint64_t size = 0;
};

asset release_asset(const json & item) {
    const auto name = item.at("name").get<std::string>();
    const auto url = item.at("browser_download_url").get<std::string>();
    const auto digest = item.value("digest", std::string{});
    const auto size = item.at("size").get<int64_t>();
    if (url.rfind(asset_url, 0) != 0 || url.substr(url.find_last_of('/') + 1) != name ||
            digest.rfind("sha256:", 0) != 0 || !valid_sha256(digest.substr(7)) || size < 1 || uint64_t(size) > binary_limit) {
        throw std::runtime_error("The release asset has invalid provenance or size.");
    }
    return {name, url, digest.substr(7), uint64_t(size)};
}

std::string read_https(const std::string & url, uint64_t limit) {
    std::string fetched;
    https_download(url, [&](const char * bytes, size_t len) {
        if (len > limit - fetched.size()) throw std::runtime_error("The engine catalog is too large.");
        fetched.append(bytes, len);
    });
    return fetched;
}

json available_packages() {
    const auto releases = json::parse(read_https(release_api, 12 * 1024 * 1024));
    auto result = json::array();
    std::unordered_set<std::string> seen;
    for (const auto & release : releases) {
        if (release.value("draft", true)) continue;
        const auto & assets = release.at("assets");
        for (const auto & item : assets) {
            const auto name = item.value("name", std::string{});
            constexpr const char * prefix = "spark-edge-engine-win-x64-";
            const auto begin = std::char_traits<char>::length(prefix);
            if (name.rfind(prefix, 0) != 0 || name.size() < begin + 5 || name.substr(name.size() - 5) != ".json") continue;
            const auto id = name.substr(begin, name.size() - begin - 5);
            if (!valid_id(id) || seen.count(id)) continue;
            const auto manifest = release_asset(item);
            if (manifest.size > manifest_limit) continue;
            const auto raw = read_https(manifest.url, manifest_limit);
            sha256_t state;
            sha256_init(&state);
            sha256_update(&state, reinterpret_cast<const unsigned char *>(raw.data()), raw.size());
            if (raw.size() != manifest.size || hex_digest(state) != manifest.sha256) {
                throw std::runtime_error("The engine manifest digest does not match its release.");
            }
            auto metadata = json::parse(raw);
            if (metadata.at("schema") != 1 || metadata.at("id") != id || metadata.at("files").empty() ||
                    metadata.at("files").size() > 32) throw std::runtime_error("This engine index is incompatible.");
            auto files = json::array();
            std::unordered_set<std::string> names;
            uint64_t total_bytes = 0;
            for (const auto & file : metadata.at("files")) {
                const auto binary = file.at("name").get<std::string>();
                auto lower = binary;
                std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
                if (!binary_name(binary) || !names.insert(lower).second) {
                    throw std::runtime_error("The engine manifest contains an invalid file name.");
                }
                const auto expected = name.substr(0, name.size() - 5) + "-" + binary;
                const auto found = std::find_if(assets.begin(), assets.end(), [&](const json & row) {
                    return row.value("name", std::string{}) == expected;
                });
                if (found == assets.end()) throw std::runtime_error("A required engine asset is missing.");
                const auto binary_asset = release_asset(*found);
                if (file.at("sha256") != binary_asset.sha256 || file.at("size") != binary_asset.size) {
                    throw std::runtime_error("An engine file does not match its release manifest.");
                }
                files.push_back({{"name", binary}, {"sha256", binary_asset.sha256}, {"size", binary_asset.size}, {"url", binary_asset.url}});
                total_bytes += binary_asset.size;
            }
            if (!names.count("llama-server.exe")) throw std::runtime_error("The engine executable is missing.");
            result.push_back({{"id", id}, {"backend", id.substr(0, id.find('-'))}, {"bytes", total_bytes},
                {"router_api", metadata.value("router_api", 1)}, {"files", files}});
            seen.insert(id);
        }
    }
    return result;
}

}

engine_packages::engine_packages(std::filesystem::path base) : root(std::move(base)) {}

json engine_packages::catalog() const {
    auto result = available_packages();
    for (auto & row : result) row.erase("files");
    return result;
}

json engine_packages::installed() const {
    using installed_entry = std::pair<std::filesystem::file_time_type, std::string>;
    std::vector<installed_entry> entries;
    std::error_code error;
    std::filesystem::directory_iterator entry(root, error);
    for (const auto & node : entry) {
        const auto id = node.path().filename().u8string();
        const auto receipt = node.path() / "engine.json";
        if (valid_id(id) && node.is_directory(error) && std::filesystem::is_regular_file(receipt, error)) {
            entries.emplace_back(std::filesystem::last_write_time(receipt, error), id);
        }
    }
    std::sort(entries.rbegin(), entries.rend());
    auto result = json::array();
    for (const auto & entry : entries) {
        const auto & id = entry.second;
        result.push_back({{"id", id}, {"backend", id.substr(0, id.find('-'))}});
    }
    return result;
}

bool engine_packages::supports_worker_fallback(const std::string & id) const {
    if (id.empty()) return true;
    if (!valid_id(id)) return false;
    std::ifstream receipt(root / id / "engine.json", std::ios::binary);
    if (!receipt) return false;
    const auto package = json::parse(receipt, nullptr, false);
    return package.is_object() && package.contains("router_api") && package["router_api"].is_number_integer() &&
           package["router_api"] >= 2;
}

std::filesystem::path engine_packages::executable(const std::string & id) const {
    if (!valid_id(id)) throw std::runtime_error("Choose a valid engine package.");
    const auto directory = root / id;
    std::ifstream receipt(directory / "engine.json", std::ios::binary);
    if (!receipt) throw std::runtime_error("This engine package is not installed.");
    const auto package = json::parse(receipt);
    if (package.at("id") != id) throw std::runtime_error("The engine receipt does not match its package.");
    bool has_server = false;
    for (const auto & file : package.at("files")) {
        const auto name = file.at("name").get<std::string>();
        if (!binary_name(name) || file_digest(directory / name) != file.at("sha256").get<std::string>()) {
            throw std::runtime_error("An installed engine file failed its digest check.");
        }
        has_server |= name == "llama-server.exe";
    }
    if (!has_server) throw std::runtime_error("The installed engine is incomplete.");
    return directory / "llama-server.exe";
}

void engine_packages::download(const std::string & id) const {
    if (!valid_id(id)) throw std::runtime_error("Choose an engine from the release catalog.");
    const auto target = root / id;
    if (std::filesystem::exists(target)) {
        executable(id);
        return;
    }
    const auto catalog = available_packages();
    const auto candidate = std::find_if(catalog.begin(), catalog.end(), [&](const json & row) { return row.at("id") == id; });
    if (candidate == catalog.end()) throw std::runtime_error("This engine is no longer in the release catalog.");
    std::filesystem::create_directories(root);
    if (std::filesystem::space(root).available < candidate->at("bytes").get<uint64_t>() + 64 * 1024 * 1024) {
        throw std::runtime_error("There is not enough disk space for this engine.");
    }

    const auto temporary = root / (id + ".partial");
    std::filesystem::create_directories(temporary);
    auto package = *candidate;
    for (auto & file : package.at("files")) {
        const auto destination = temporary / file.at("name").get<std::string>();
        const auto length = file.at("size").get<uint64_t>();
        uint64_t written = 0;
        sha256_t state;
        sha256_init(&state);
        std::ofstream output(destination, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("Cannot open the staged engine file.");
        https_download(file.at("url").get<std::string>(), [&](const char * data, size_t size) {
            if (size > length - written) throw std::runtime_error("An engine file exceeded its declared size.");
            if (!output.write(data, size)) throw std::runtime_error("Could not save the engine file.");
            sha256_update(&state, reinterpret_cast<const unsigned char *>(data), size);
            written += size;
        });
        output.close();
        if (!output || written != length || hex_digest(state) != file.at("sha256").get<std::string>()) {
            throw std::runtime_error("A downloaded engine file failed verification.");
        }
        file.erase("url");
    }
    atomic_write(temporary / "engine.json", package.dump(2) + '\n', false);
    std::filesystem::rename(temporary, target);
}

}
