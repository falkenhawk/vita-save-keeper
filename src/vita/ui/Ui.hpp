#pragma once

#include "core/SaveRecord.hpp"

#include <cstddef>
#include <string>
#include <vector>

struct vita2d_pgf;

namespace vsm::vita {

class Ui {
public:
  bool initialize();
  void shutdown();
  void draw(const std::vector<SaveRecord> &saves, std::size_t selected_save,
            const std::string &status_message);

private:
  void draw_header() const;
  void draw_title_grid(const std::vector<SaveRecord> &saves, std::size_t selected_save) const;
  void draw_backup_panel(const std::vector<SaveRecord> &saves, std::size_t selected_save,
                         const std::string &status_message) const;
  void draw_footer() const;

  vita2d_pgf *font_{};
};

} // namespace vsm::vita
