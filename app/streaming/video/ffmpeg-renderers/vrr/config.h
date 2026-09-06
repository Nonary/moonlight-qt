#pragma once
#include "timing.h"
#include <string>

namespace Vrr {
bool loadConfig(const std::string& path, Config& config, std::string& error);
}
