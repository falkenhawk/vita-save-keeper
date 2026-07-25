#include "core/AppSettings.hpp"

namespace vsm {

AppSettings parse_app_settings(const std::string &text) {
  AppSettings settings;
  std::size_t start = 0;
  while (start < text.size()) {
    std::size_t end = text.find('\n', start);
    if (end == std::string::npos) {
      end = text.size();
    }
    std::string line = text.substr(start, end - start);
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    const std::size_t equals = line.find('=');
    if (equals != std::string::npos) {
      const std::string key = line.substr(0, equals);
      const std::string value = line.substr(equals + 1);
      if (key == "sort") {
        settings.sort_mode = save_sort_mode_from_string(value);
      } else if (key == "cleaned_empty_backup_folders") {
        settings.cleaned_empty_backup_folders = value == "1";
      }
    }
    start = end + 1;
  }
  return settings;
}

std::string serialize_app_settings(const AppSettings &settings) {
  std::string text = "sort=" + save_sort_mode_to_string(settings.sort_mode) + "\n";
  if (settings.cleaned_empty_backup_folders) {
    text += "cleaned_empty_backup_folders=1\n";
  }
  return text;
}

} // namespace vsm
