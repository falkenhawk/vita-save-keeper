#pragma once

struct vita2d_pgf;

namespace vsm::vita {

class Ui {
public:
  bool initialize();
  void shutdown();
  void draw();

private:
  void draw_header() const;
  void draw_title_grid() const;
  void draw_backup_panel() const;
  void draw_footer() const;

  vita2d_pgf *font_{};
};

} // namespace vsm::vita
