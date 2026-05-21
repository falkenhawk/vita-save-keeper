#pragma once

#include "core/SaveRecord.hpp"
#include "vita/ui/Ui.hpp"

#include <cstddef>
#include <vector>

namespace vsm::vita {

class App {
public:
  int run();

private:
  std::vector<SaveRecord> saves_;
  std::size_t selected_save_{};
  Ui ui_;
};

} // namespace vsm::vita
