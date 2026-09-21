// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright (c) 2026 SparkMoE contributors.
#pragma once
#include <filesystem>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>

namespace sparkmoe {
class downloads {
public:
    downloads(const std::filesystem::path & data, std::function<void(const std::filesystem::path &)> imported);
    ~downloads();
    nlohmann::ordered_json dispatch(const std::string & action, const nlohmann::ordered_json & input);
private:
    struct impl;
    std::unique_ptr<impl> state;
};
}
