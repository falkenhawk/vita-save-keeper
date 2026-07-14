#pragma once

#include <string>
#include <vector>

namespace vsm {

struct DriveFile {
  std::string id;
  std::string name;
  // First entry of the Drive "parents" array; empty when the response did not include parents.
  std::string parent_id;
};

struct DriveFileList {
  bool ok{};
  std::vector<DriveFile> files;
  std::string next_page_token;
  std::string error;
};

// Drive folder name for a save: the normalized save key plus the game's title, so the Drive UI is
// browsable ("PCSB00456 FEZ"); falls back to the bare key when no distinct title is known. The
// matcher accepts both forms, so folders created by older versions (bare key) keep working and
// can be renamed opportunistically later.
std::string drive_save_folder_name(const std::string &save_key, const std::string &display_name);
bool drive_folder_matches_save(const std::string &folder_name, const std::string &save_key);
std::string build_drive_rename_metadata_json(const std::string &name);

std::string build_drive_folder_metadata_json(const std::string &folder_name,
                                             const std::string &parent_id);
std::string build_drive_upload_metadata_json(const std::string &file_name,
                                             const std::string &parent_id);
std::string build_drive_sidecar_upload_metadata_json(const std::string &file_name,
                                                     const std::string &parent_id,
                                                     const std::string &archive_file_id);
std::string build_drive_find_folder_query(const std::string &folder_name,
                                          const std::string &parent_id);
std::string build_drive_find_sidecar_by_archive_query(const std::string &parent_id,
                                                       const std::string &archive_file_id);
std::string build_drive_find_child_by_name_query(const std::string &parent_id,
                                                  const std::string &name);
std::string build_drive_list_children_query(const std::string &parent_id);
// Whole-account listings used by the startup index sync: every folder and every non-folder file
// the app can see (drive.file scope limits both to files this app created), with parents included
// so backups can be grouped by their save folder. page_token may be empty for the first page.
std::string build_drive_list_all_folders_query(const std::string &page_token);
std::string build_drive_list_all_files_query(const std::string &page_token);
DriveFileList parse_drive_file_list(const std::string &json);

} // namespace vsm
