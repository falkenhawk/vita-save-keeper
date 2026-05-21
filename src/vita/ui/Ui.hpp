#pragma once

#include "core/SaveRecord.hpp"

#include <cstddef>
#include <map>
#include <string>
#include <vector>

struct vita2d_pgf;
struct vita2d_texture;

namespace vsm::vita {

class Ui {
public:
  bool initialize();
  void shutdown();
  void draw(const std::vector<SaveRecord> &saves, std::size_t selected_save,
            const std::vector<std::string> &remote_backups,
            const std::vector<std::string> &local_backups, std::size_t selected_backup,
            bool restore_confirmation_pending, bool google_connected, bool google_auth_pending,
            const std::string &google_verification_url, const std::string &google_user_code,
            const std::string &status_message);

private:
  void draw_header(bool google_connected, bool google_auth_pending) const;
  void draw_title_grid(const std::vector<SaveRecord> &saves, std::size_t selected_save) const;
  void draw_backup_panel(const std::vector<SaveRecord> &saves, std::size_t selected_save,
                         const std::vector<std::string> &remote_backups,
                         const std::vector<std::string> &local_backups,
                         std::size_t selected_backup, bool restore_confirmation_pending,
                         const std::string &google_verification_url,
                         const std::string &google_user_code,
                         const std::string &status_message) const;
  void draw_footer() const;
  vita2d_texture *load_icon_texture(const std::string &path) const;

  vita2d_pgf *font_{};
  mutable std::map<std::string, vita2d_texture *> icon_cache_;
};

} // namespace vsm::vita
