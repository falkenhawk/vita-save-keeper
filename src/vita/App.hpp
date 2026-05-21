#pragma once

#include "vita/ui/Ui.hpp"

namespace vsm::vita {

class App {
public:
  int run();

private:
  Ui ui_;
};

} // namespace vsm::vita
