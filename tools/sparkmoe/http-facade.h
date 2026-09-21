// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright (c) 2026 SparkMoE contributors.
#pragma once
#include "httplib.h"
#include <atomic>

namespace sparkmoe {
bool serve_ui_asset(const httplib::Request & request, httplib::Response & response);
void proxy_request(const httplib::Request & request, httplib::Response & response, int port, std::atomic<int> * active = nullptr);
}
