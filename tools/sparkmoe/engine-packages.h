// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright (c) 2026 SparkMoE contributors.
#pragma once

#include <filesystem>
#include <string>
#include <nlohmann/json.hpp>

namespace sparkmoe {
class engine_packages {
public:
    explicit engine_packages(std::filesystem::path root);
    nlohmann::ordered_json catalog() const;
    nlohmann::ordered_json installed() const;
    void download(const std::string & id) const;
    std::filesystem::path executable(const std::string & id) const;

private:
    const std::filesystem::path root;
};
}
