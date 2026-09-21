// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright (c) 2026 SparkMoE contributors.

#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <nlohmann/json.hpp>

namespace sparkmoe {

struct connection_context {
    std::filesystem::path data_directory;
    nlohmann::ordered_json models = nlohmann::ordered_json::array();
    std::string api_base_url = "http://127.0.0.1:18080/v1";
    std::string api_key;
    std::function<void(const std::string &)> ensure_model_ready;
};

nlohmann::ordered_json connections_dispatch(const std::string & action,
    const nlohmann::ordered_json & input, const connection_context & context);

}
