// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright (c) 2026 SparkMoE contributors.
#pragma once

#include <filesystem>
#include <string>
#include <nlohmann/json.hpp>

namespace sparkmoe {
using library_json = nlohmann::ordered_json;
library_json discover_models(const std::filesystem::path & path);
library_json inspect_model(const std::filesystem::path & path);
std::string model_slug(const std::string & name);
void atomic_write(const std::filesystem::path & path, const std::string & content, bool backup = true);
}
