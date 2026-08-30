// SPDX-License-Identifier: MIT
// SparkMoE integration extracted from MIT-licensed upstream patches.

#pragma once
#include "arg.h"
#include <vector>

std::vector<common_arg> sparkmoe_options();
void sparkmoe_normalize_params(common_params & params);
