// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright (c) 2026 SparkMoE contributors.
#include "http-facade.h"
#include "ui.h"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace sparkmoe {
bool serve_ui_asset(const httplib::Request & request, httplib::Response & response) {
    const std::string name = request.path == "/" ? "index.html" : request.path.substr(1);
    const auto * asset = llama_ui_find_asset(name);
    if (!asset) return false;
    const bool refresh = name == "index.html" || name == "sw.js" || name.find("manifest") != std::string::npos || name == "version.json";
    response.set_header("Cache-Control", refresh ? "no-cache" : "public, max-age=31536000, immutable");
    response.set_header("ETag", asset->etag);
    response.set_header("X-Content-Type-Options", "nosniff");
    response.set_header("Referrer-Policy", "same-origin");
    if (name == "index.html") {
        response.set_header("Cross-Origin-Embedder-Policy", "require-corp");
        response.set_header("Cross-Origin-Opener-Policy", "same-origin");
    }
    if (request.get_header_value("If-None-Match") == asset->etag) { response.status = 304; return true; }
    if (llama_ui_use_gzip()) {
        if (request.get_header_value("Accept-Encoding").find("gzip") == std::string::npos) {
            response.status = 406;
            response.set_content("This interface requires gzip support.", "text/plain");
            return true;
        }
        response.set_header("Content-Encoding", "gzip");
        response.set_header("Vary", "Accept-Encoding");
    }
    response.set_content(reinterpret_cast<const char *>(asset->data), asset->size, asset->type);
    return true;
}

namespace {
struct transfer {
    explicit transfer(int port, std::atomic<int> * counter) : client("127.0.0.1", port), active(counter) {
        if (active) ++*active;
        client.set_connection_timeout(2);
        client.set_read_timeout(600);
        client.set_write_timeout(60);
        client.set_decompress(false);
    }
    ~transfer() {
        { std::lock_guard<std::mutex> lock(mutex); cancelled = true; }
        changed.notify_all();
        client.stop();
        if (worker.joinable()) worker.join();
        if (active) --*active;
    }
    httplib::Client client;
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<std::string> chunks;
    size_t queued = 0;
    httplib::Response head;
    bool headers_ready = false, done = false, cancelled = false, failed = false;
    std::thread worker;
    std::atomic<int> * active;
};
bool hop_header(std::string name) {
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return name == "host" || name == "connection" || name == "transfer-encoding" || name == "content-length" ||
        name == "keep-alive" || name == "upgrade" || name == "proxy-authorization" || name == "proxy-authenticate" || name == "te" || name == "trailer";
}
}

void proxy_request(const httplib::Request & incoming, httplib::Response & outgoing, int port, std::atomic<int> * active) {
    const auto state = std::make_shared<transfer>(port, active);
    httplib::Request request;
    request.method = incoming.method;
    request.path = incoming.target.empty() ? incoming.path : incoming.target;
    request.body = incoming.body;
    for (const auto & header : incoming.headers) if (!hop_header(header.first)) request.headers.insert(header);
    request.headers.erase("Accept-Encoding");
    request.headers.emplace("Accept-Encoding", "identity");
    request.response_handler = [raw = state.get()](const httplib::Response & response) {
        std::lock_guard<std::mutex> lock(raw->mutex);
        raw->head.status = response.status;
        raw->head.headers = response.headers;
        raw->headers_ready = true;
        raw->changed.notify_all();
        return !raw->cancelled;
    };
    request.content_receiver = [raw = state.get()](const char * data, size_t size, uint64_t, uint64_t) {
        std::unique_lock<std::mutex> lock(raw->mutex);
        raw->changed.wait(lock, [&]() { return raw->cancelled || raw->queued < 1048576; });
        if (raw->cancelled) return false;
        raw->chunks.emplace_back(data, size);
        raw->queued += size;
        raw->changed.notify_all();
        return true;
    };
    state->worker = std::thread([raw = state.get(), request = std::move(request)]() {
        const auto result = raw->client.send(request);
        std::lock_guard<std::mutex> lock(raw->mutex);
        raw->done = true;
        raw->failed = !result;
        raw->changed.notify_all();
    });
    {
        std::unique_lock<std::mutex> lock(state->mutex);
        while (!state->headers_ready && !state->done) {
            state->changed.wait_for(lock, std::chrono::milliseconds(100));
            if (incoming.is_connection_closed()) { state->cancelled = true; state->changed.notify_all(); return; }
        }
        if (!state->headers_ready) {
            outgoing.status = 503;
            outgoing.set_content(R"({"error":{"message":"The model server is unavailable. Open Spark Edge to start it.","type":"server_unavailable"}})", "application/json");
            return;
        }
        outgoing.status = state->head.status;
        for (const auto & header : state->head.headers) if (!hop_header(header.first)) outgoing.headers.insert(header);
    }
    const auto content_type = state->head.get_header_value("Content-Type", "application/octet-stream");
    outgoing.set_chunked_content_provider(content_type, [state](size_t, httplib::DataSink & sink) {
        std::string chunk;
        {
            std::unique_lock<std::mutex> lock(state->mutex);
            while (state->chunks.empty() && !state->done && !state->cancelled) {
                state->changed.wait_for(lock, std::chrono::milliseconds(100));
                if (!sink.is_writable()) { state->cancelled = true; state->changed.notify_all(); lock.unlock(); state->client.stop(); return false; }
            }
            if (state->cancelled) return false;
            if (state->chunks.empty()) { if (state->failed) return false; sink.done(); return true; }
            chunk = std::move(state->chunks.front());
            state->chunks.pop_front();
            state->queued -= chunk.size();
            state->changed.notify_all();
        }
        return sink.write(chunk.data(), chunk.size());
    });
}
}
