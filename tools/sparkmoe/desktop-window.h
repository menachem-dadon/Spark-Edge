// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright (c) 2026 SparkMoE contributors.

#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace sparkmoe {

class desktop_window {
public:
    struct options {
        std::string url;
        std::filesystem::path user_data_directory;
        std::function<bool()> keep_in_background;
        std::function<bool()> use_hebrew;
        std::function<bool()> has_active_requests;
        std::function<void()> request_shutdown;
        std::function<void(const std::string &)> log;
        std::function<void()> browser_fallback;
    };

    desktop_window();
    ~desktop_window();
    desktop_window(const desktop_window &) = delete;
    desktop_window & operator=(const desktop_window &) = delete;

    // Call start and stop from the owner thread. Callbacks must not join this window's thread.
    bool start(options config);
    bool show();
    void stop();
    void * native_handle() const;

private:
    struct implementation;
    std::unique_ptr<implementation> impl_;
};

}
