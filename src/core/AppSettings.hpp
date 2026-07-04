#pragma once

#include "core/SaveScanner.hpp"

#include <string>

namespace vsm {

struct AppSettings {
  SaveSortMode sort_mode{SaveSortMode::Name};
};

// settings.txt is plain key=value lines; unknown keys are ignored so older builds can read
// files written by newer ones.
AppSettings parse_app_settings(const std::string &text);
std::string serialize_app_settings(const AppSettings &settings);

} // namespace vsm
