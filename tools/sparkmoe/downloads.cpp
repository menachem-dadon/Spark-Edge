// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright (c) 2026 SparkMoE contributors.
#include "downloads.h"
#include "model-library.h"
#include "download.h"
#include <atomic>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <regex>
#include <thread>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#endif

namespace sparkmoe {
using json = nlohmann::ordered_json;
namespace {
std::string encode(const std::string & value, bool slash = false) {
    std::string result;
    const char hex[] = "0123456789ABCDEF";
    for (unsigned char c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || (slash && c == '/')) result += c;
        else { result += '%'; result += hex[c >> 4]; result += hex[c & 15]; }
    }
    return result;
}
void validate_repo(const std::string & repo) {
    if (!std::regex_match(repo, std::regex("[A-Za-z0-9_-]+/[A-Za-z0-9_.-]+")) || repo.size() > 180) throw std::runtime_error("Enter a Hugging Face repository as owner/model.");
}
#if defined(_WIN32)
std::wstring wide(const std::string & value) {
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (!count && !value.empty()) throw std::runtime_error("Invalid URL encoding.");
    std::wstring result(count, 0);
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), count);
    return result;
}
struct internet_handle {
    HINTERNET value = nullptr;
    explicit internet_handle(HINTERNET handle) : value(handle) { if (!value) throw std::runtime_error("Cannot connect. Check the network and try again."); }
    ~internet_handle() { WinHttpCloseHandle(value); }
};
std::string header(HINTERNET request, DWORD name) {
    DWORD size = 0;
    WinHttpQueryHeaders(request, name, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &size, nullptr);
    if (!size || size > 65536) return {};
    std::wstring value(size / sizeof(wchar_t), 0);
    if (!WinHttpQueryHeaders(request, name, WINHTTP_HEADER_NAME_BY_INDEX, value.data(), &size, nullptr)) return {};
    value.resize(size / sizeof(wchar_t));
    std::string result;
    for (const auto c : value) {
        if (c > 127 || c == '\r' || c == '\n') throw std::runtime_error("Invalid download response header.");
        result += static_cast<char>(c);
    }
    return result;
}
void get_https(const std::string & url, const std::string & headers,
        const std::function<bool(int, const std::string &, const std::string &, const std::string &)> & head,
        const std::function<bool(const char *, size_t)> & body) {
    auto address = wide(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = DWORD(-1);
    if (!WinHttpCrackUrl(address.c_str(), static_cast<DWORD>(address.size()), 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS) throw std::runtime_error("An HTTPS download URL is required.");
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    const std::wstring path = std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength) + std::wstring(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    internet_handle session(WinHttpOpen(L"SparkMoE", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    WinHttpSetTimeouts(session.value, 10000, 10000, 10000, 10000);
    internet_handle connection(WinHttpConnect(session.value, host.c_str(), parts.nPort, 0));
    internet_handle request(WinHttpOpenRequest(connection.value, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(request.value, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
    DWORD auth = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
    WinHttpSetOption(request.value, WINHTTP_OPTION_AUTOLOGON_POLICY, &auth, sizeof(auth));
    const auto extra = wide(headers);
    if (!WinHttpSendRequest(request.value, extra.c_str(), static_cast<DWORD>(extra.size()), nullptr, 0, 0, 0) || !WinHttpReceiveResponse(request.value, nullptr)) throw std::runtime_error("Download connection failed. Check the network and try again.");
    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &size, nullptr);
    if (!head(static_cast<int>(status), header(request.value, WINHTTP_QUERY_CONTENT_LENGTH), header(request.value, WINHTTP_QUERY_ETAG), header(request.value, WINHTTP_QUERY_CONTENT_RANGE))) return;
    char buffer[65536];
    DWORD received = 0;
    while (true) {
        if (!WinHttpReadData(request.value, buffer, sizeof(buffer), &received)) throw std::runtime_error("The download was interrupted. Resume to try again.");
        if (!received || !body(buffer, received)) return;
    }
}
#else
void get_https(const std::string &, const std::string &,
        const std::function<bool(int, const std::string &, const std::string &, const std::string &)> &,
        const std::function<bool(const char *, size_t)> &) {
    throw std::runtime_error("In-app downloads are available in the Windows desktop build. Download the GGUF files and add their folder.");
}
#endif
json remote_json(const std::string & url) {
    std::string content;
    get_https(url, "Accept: application/json\r\n", [](int status, const auto &, const auto &, const auto &) {
        if (status != 200) throw std::runtime_error("Hugging Face returned HTTP " + std::to_string(status) + ". Check the repository and access requirements.");
        return true;
    }, [&](const char * data, size_t size) {
        if (content.size() + size > 8 * 1024 * 1024) throw std::runtime_error("The repository listing is too large. Paste a direct GGUF download link.");
        content.append(data, size);
        return true;
    });
    return json::parse(content);
}
std::vector<std::string> download_urls(const std::string & url) {
    if (url.size() > 2048 || !std::regex_match(url, std::regex(R"(https://huggingface\.co/[A-Za-z0-9_-]+/[A-Za-z0-9_.-]+/resolve/[A-Za-z0-9_.-]+/[A-Za-z0-9_./%+-]+\.gguf)")) || url.find("..") != std::string::npos) throw std::runtime_error("Paste a public Hugging Face GGUF download link ending in .gguf.");
    std::smatch split;
    if (std::regex_search(url, split, std::regex("-[0-9]{5}-of-([0-9]{5})\\.gguf$")) && std::stoi(split[1].str()) > 1024) throw std::runtime_error("This download has too many model parts.");
    const auto parts = common_download_get_all_parts(url);
    if (parts.size() > 1024) throw std::runtime_error("This download has too many model parts.");
    return parts;
}
}

void https_download(const std::string & url, const std::function<void(const char *, size_t)> & consume) {
    get_https(url, "Accept: application/json, application/octet-stream\r\n",
        [](int status, const auto &, const auto &, const auto &) {
            if (status != 200) throw std::runtime_error("Download returned HTTP " + std::to_string(status));
            return true;
        },
        [&](const char * bytes, size_t count) { consume(bytes, count); return true; });
}

struct downloads::impl {
    std::filesystem::path directory, settings;
    std::function<void(const std::filesystem::path &)> imported;
    std::mutex mutex;
    std::condition_variable changed;
    json jobs = json::array();
    std::atomic<bool> stopping{false}, paused{false};
    std::string active;
    std::thread worker;
    uint64_t sequence = 0;

    impl(const std::filesystem::path & data, std::function<void(const std::filesystem::path &)> callback) : directory(data / "models"), settings(data / "downloads.json"), imported(std::move(callback)) {
        if (std::filesystem::exists(settings)) {
            try {
                std::ifstream input(settings);
                auto saved = json::parse(input);
                sequence = saved.value("sequence", uint64_t(0));
                for (auto job : saved.value("jobs", json::array())) {
                    if (!std::regex_match(job.value("id", ""), std::regex("download-[0-9]+"))) continue;
                    download_urls(job.value("url", ""));
                    if (job.value("status", "") == "downloading" || job.value("status", "") == "queued") job["status"] = "paused";
                    jobs.push_back(std::move(job));
                }
            } catch (...) { std::filesystem::copy_file(settings, settings.parent_path() / "downloads-recovery.json", std::filesystem::copy_options::skip_existing); }
        }
        worker = std::thread([this]() { run(); });
    }
    ~impl() { stopping = true; changed.notify_all(); if (worker.joinable()) worker.join(); }
    void save() { atomic_write(settings, json({{"sequence", sequence}, {"jobs", jobs}}).dump(2)); }
    json & find(const std::string & id) {
        for (auto & job : jobs) if (job["id"] == id) return job;
        throw std::runtime_error("Download not found.");
    }
    void update(const std::string & id, const json & fields, bool persist = false) {
        std::lock_guard<std::mutex> lock(mutex);
        find(id).update(fields);
        if (persist) save();
    }
    void download(const json & job) {
        const auto id = job["id"].get<std::string>();
        const auto folder = directory / id;
        std::filesystem::create_directories(folder);
        const auto urls = download_urls(job["url"].get<std::string>());
        uint64_t complete = 0;
        std::filesystem::path first;
        for (const auto & url : urls) {
            if (stopping || paused) break;
            auto name = url.substr(url.rfind('/') + 1);
            if (name.size() > 220 || name.find('%') != std::string::npos || name.find_first_of("\\:") != std::string::npos) throw std::runtime_error("This filename is not supported. Download it in your browser and add the file.");
            const auto target = folder / std::filesystem::u8path(name);
            if (first.empty()) first = target;
            if (std::filesystem::is_regular_file(target)) { inspect_model(target); complete += std::filesystem::file_size(target); continue; }
            const auto partial = folder / std::filesystem::u8path(name + ".partial");
            const auto etag_path = folder / std::filesystem::u8path(name + ".etag");
            uint64_t offset = std::filesystem::exists(partial) ? std::filesystem::file_size(partial) : 0;
            std::string etag;
            { std::ifstream saved(etag_path); std::getline(saved, etag); }
            std::string headers = "Accept-Encoding: identity\r\n";
            if (offset && !etag.empty()) headers += "Range: bytes=" + std::to_string(offset) + "-\r\nIf-Range: " + etag + "\r\n";
            else offset = 0;
            std::ofstream output;
            uint64_t expected = 0, received = offset;
            bool received_response = false;
            for (int attempt = 0; attempt < 2; ++attempt) {
                bool restart = false;
                get_https(url, headers, [&](int status, const auto & length, const auto & version, const auto & range) {
                if (status == 416 && offset) {
                    if (!etag.empty() && version == etag && range == "bytes */" + std::to_string(offset)) {
                        inspect_model(partial);
                        expected = received = offset;
                        return false;
                    }
                    restart = true;
                    return false;
                }
                if (status != 200 && status != 206) throw std::runtime_error("Download returned HTTP " + std::to_string(status) + ". Check access or resume later.");
                if (status == 200) offset = received = 0;
                if (status == 206 && (offset == 0 || range.rfind("bytes " + std::to_string(offset) + "-", 0) != 0 || (!etag.empty() && version != etag))) { restart = true; return false; }
                expected = length.empty() ? 0 : std::stoull(length) + offset;
                std::error_code error;
                const auto space = std::filesystem::space(folder, error);
                if (!error && expected > offset && space.available < expected - offset + 64 * 1024 * 1024) throw std::runtime_error("There is not enough free disk space for this download.");
                output.open(partial, std::ios::binary | (offset ? std::ios::app : std::ios::trunc));
                if (!output) throw std::runtime_error("Cannot write the model download.");
                atomic_write(etag_path, version, false);
                update(id, {{"downloaded_bytes", complete + received}, {"total_bytes", complete + expected}});
                return !stopping && !paused;
            }, [&](const char * data, size_t size) {
                if (stopping || paused) return false;
                if (!output.write(data, static_cast<std::streamsize>(size))) throw std::runtime_error("Cannot save the model. Check free disk space.");
                received += size;
                update(id, {{"downloaded_bytes", complete + received}, {"total_bytes", complete + expected}});
                return true;
            });
                if (!restart) { received_response = true; break; }
                headers = "Accept-Encoding: identity\r\n";
                offset = received = 0;
            }
            if (!received_response) throw std::runtime_error("The server did not return a complete download. Try again later.");
            if (output.is_open()) { output.flush(); if (!output) throw std::runtime_error("Cannot finish writing the model file."); output.close(); }
            if (stopping || paused) break;
            if (expected && received != expected) throw std::runtime_error("Download is incomplete. Resume to try again.");
            inspect_model(partial);
            std::filesystem::rename(partial, target);
            complete += received;
        }
        if (stopping || paused) { update(id, {{"status", "paused"}}, true); return; }
        const auto models = discover_models(first);
        if (models.empty() || !models[0].value("available", false)) throw std::runtime_error("Model files are incomplete. Download all model parts.");
        imported(first);
        update(id, {{"status", "completed"}, {"path", first.u8string()}, {"downloaded_bytes", complete}, {"total_bytes", complete}, {"error", ""}}, true);
    }
    void run() {
        while (!stopping) {
            json job;
            {
                std::unique_lock<std::mutex> lock(mutex);
                changed.wait(lock, [&]() { return stopping || std::any_of(jobs.begin(), jobs.end(), [](const auto & row) { return row.value("status", "") == "queued"; }); });
                if (stopping) return;
                for (auto & row : jobs) if (row.value("status", "") == "queued") { row["status"] = "downloading"; job = row; break; }
                active = job["id"].get<std::string>();
                paused = false;
            }
            try { download(job); }
            catch (const std::exception & error) {
                try { update(job["id"].get<std::string>(), {{"status", stopping || paused ? "paused" : "failed"}, {"error", error.what()}}, true); } catch (...) {}
            }
            { std::lock_guard<std::mutex> lock(mutex); active.clear(); }
        }
    }
};

downloads::downloads(const std::filesystem::path & data, std::function<void(const std::filesystem::path &)> callback) : state(std::make_unique<impl>(data, std::move(callback))) {}
downloads::~downloads() = default;
json downloads::dispatch(const std::string & action, const json & input) {
    if (action == "search") {
        const auto query = input.value("q", "");
        if (query.size() < 2 || query.size() > 160) throw std::runtime_error("Enter at least two characters to search.");
        const auto rows = remote_json("https://huggingface.co/api/models?filter=gguf&limit=20&sort=downloads&direction=-1&search=" + encode(query));
        json models = json::array();
        for (const auto & row : rows) models.push_back({{"id", row.value("id", "")}, {"downloads", row.value("downloads", 0)}});
        return {{"models", models}};
    }
    if (action == "catalog") {
        const auto repo = input.value("repo", "");
        validate_repo(repo);
        const auto model = remote_json("https://huggingface.co/api/models/" + repo + "?blobs=true");
        json files = json::array();
        const auto revision = model.value("sha", "main");
        for (const auto & item : model.value("siblings", json::array())) {
            const auto name = item.value("rfilename", "");
            if (name.size() < 5 || name.substr(name.size() - 5) != ".gguf") continue;
            if (name.find("-of-") != std::string::npos && name.find("-00001-of-") == std::string::npos) continue;
            files.push_back({{"name", name}, {"url", "https://huggingface.co/" + repo + "/resolve/" + revision + "/" + encode(name, true)}, {"size_bytes", item.value("size", uint64_t(0))}});
        }
        return {{"repo", repo}, {"files", files}, {"model_card_url", "https://huggingface.co/" + repo}};
    }
    std::lock_guard<std::mutex> lock(state->mutex);
    if (action == "add") {
        auto url = input.value("url", "");
        const auto query = url.find('?');
        if (query != std::string::npos) url.resize(query);
        const auto urls = download_urls(url);
        if (state->jobs.size() >= 256) throw std::runtime_error("The download history is full.");
        for (const auto & job : state->jobs) if (job["url"] == url) throw std::runtime_error("This download is already in the list.");
        const auto id = "download-" + std::to_string(++state->sequence);
        state->jobs.push_back({{"id", id}, {"url", url}, {"filename", urls[0].substr(urls[0].rfind('/') + 1)}, {"status", "queued"}, {"downloaded_bytes", 0}, {"total_bytes", 0}, {"error", ""}, {"path", ""}});
        try { state->save(); } catch (...) { state->jobs.erase(state->jobs.end() - 1); throw; }
        state->changed.notify_all();
    } else if (action == "pause" || action == "resume") {
        auto & job = state->find(input.value("id", ""));
        if (job["status"] == "completed") throw std::runtime_error("This download is complete.");
        if (action == "pause") {
            if (state->active == job["id"]) state->paused = true;
            else job["status"] = "paused";
        } else {
            if (state->active == job["id"]) throw std::runtime_error("Wait for the download to pause before resuming.");
            job["status"] = "queued"; job["error"] = "";
            state->changed.notify_all();
        }
        state->save();
    } else if (action != "list") throw std::runtime_error("Unknown download action.");
    return {{"jobs", state->jobs}, {"directory", state->directory.u8string()}};
}
}
