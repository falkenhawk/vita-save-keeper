#include "core/SyncPlan.hpp"

#include <string>

namespace vsm {
namespace {

std::string count_noun(std::size_t count, const char *singular, const char *plural) {
  return std::to_string(count) + " " + (count == 1 ? singular : plural);
}

} // namespace

SyncItemPlan plan_sync_item(const SyncItemInput &input) {
  SyncItemPlan plan;
  if (!input.entries_ok) {
    // An unwalkable folder would fail the zip step the same way; skipping keeps the run from
    // stacking one timeout-style failure per file, and the summary reports it.
    plan.unreadable = true;
    return plan;
  }

  plan.create_backup = !input.folder_empty && !input.matches_existing;
  if (!input.drive_connected) {
    return plan;
  }

  if (plan.create_backup) {
    // The fresh archive gets a new timestamped name, so it cannot already exist on Drive.
    plan.will_upload = true;
  } else if (!input.newest_local.empty() && !input.newest_on_drive) {
    plan.upload_existing = input.newest_local;
    plan.will_upload = true;
  }
  return plan;
}

std::string sync_all_confirm_message(std::size_t games, const std::string &tab_label,
                                     bool drive_connected) {
  const std::string saves_noun =
      std::to_string(games) + " " + tab_label + (games == 1 ? " save" : " saves");
  // Offline needs no extra note: the header already shows it, and the missing "& upload" in the
  // verb says what this run will and will not do.
  const std::string action = drive_connected ? "Backup & upload " : "Backup ";
  const std::string scope = games == 1 ? saves_noun : "all " + saves_noun;
  return action + scope + "?";
}

std::string sync_run_summary(const SyncRunCounts &counts) {
  std::string parts;
  const auto append = [&parts](const std::string &part) {
    if (!parts.empty()) {
      parts += ", ";
    }
    parts += part;
  };
  if (counts.backed_up > 0) {
    append("backed up " + std::to_string(counts.backed_up));
  }
  if (counts.uploaded > 0) {
    append("uploaded " + std::to_string(counts.uploaded));
  }
  if (counts.up_to_date > 0) {
    append(std::to_string(counts.up_to_date) + " up to date");
  }
  if (counts.failed > 0) {
    append(std::to_string(counts.failed) + " failed");
  }

  if (counts.games_left > 0) {
    append(count_noun(counts.games_left, "game left", "games left"));
    return "Cancelled: " + parts + ".";
  }
  if (parts.empty()) {
    return "Nothing to do.";
  }
  if (counts.backed_up == 0 && counts.uploaded == 0 && counts.failed == 0) {
    return "All " + std::to_string(counts.up_to_date) + " games up to date.";
  }
  parts[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(parts[0])));
  return parts + ".";
}

std::string sync_all_hold_message(const std::string &tab_label) {
  return "Keep holding: backup & upload all " + tab_label + " saves...";
}

} // namespace vsm
