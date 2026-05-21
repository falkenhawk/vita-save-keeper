#pragma once

#include <string>
#include <vector>

namespace vsm {

struct DriveFile {
  std::string id;
  std::string name;
};

struct DriveFileList {
  bool ok{};
  std::vector<DriveFile> files;
  std::string error;
};

std::string build_drive_folder_metadata_json(const std::string &folder_name,
                                             const std::string &parent_id);
std::string build_drive_upload_metadata_json(const std::string &file_name,
                                             const std::string &parent_id);
std::string build_drive_find_folder_query(const std::string &folder_name,
                                          const std::string &parent_id);
std::string build_drive_list_children_query(const std::string &parent_id);
DriveFileList parse_drive_file_list(const std::string &json);

} // namespace vsm
