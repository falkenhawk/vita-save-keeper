#include "core/GoogleDrive.hpp"

#include "core/GoogleAuth.hpp"
#include "core/PathUtil.hpp"

#include <cstdlib>
#include <string>

namespace vsm {
namespace {

std::string json_escape(const std::string &value) {
  std::string escaped;
  for (const char ch : value) {
    if (ch == '"' || ch == '\\') {
      escaped.push_back('\\');
    }
    escaped.push_back(ch);
  }
  return escaped;
}

std::string drive_query_escape(const std::string &value) {
  std::string escaped;
  for (const char ch : value) {
    if (ch == '\'' || ch == '\\') {
      escaped.push_back('\\');
    }
    escaped.push_back(ch);
  }
  return escaped;
}

bool find_json_string_from(const std::string &json, std::size_t start, const std::string &key,
                           std::string *value, std::size_t *end_pos) {
  const std::string needle = "\"" + key + "\"";
  const std::size_t key_pos = json.find(needle, start);
  if (key_pos == std::string::npos) {
    return false;
  }
  const std::size_t colon = json.find(':', key_pos + needle.size());
  if (colon == std::string::npos) {
    return false;
  }
  const std::size_t quote = json.find('"', colon + 1);
  if (quote == std::string::npos) {
    return false;
  }

  std::string result;
  bool escaped = false;
  for (std::size_t i = quote + 1; i < json.size(); ++i) {
    const char ch = json[i];
    if (escaped) {
      result.push_back(ch);
      escaped = false;
      continue;
    }
    if (ch == '\\') {
      escaped = true;
      continue;
    }
    if (ch == '"') {
      *value = result;
      if (end_pos) {
        *end_pos = i + 1;
      }
      return true;
    }
    result.push_back(ch);
  }
  return false;
}

} // namespace

std::string drive_save_folder_name(const std::string &save_key,
                                   const std::string &display_name) {
  const std::string title = normalize_path_component(display_name);
  if (title.empty() || title == save_key) {
    return save_key;
  }
  return save_key + " " + title;
}

bool drive_folder_matches_save(const std::string &folder_name, const std::string &save_key) {
  if (folder_name == save_key) {
    return true;
  }
  return folder_name.size() > save_key.size() + 1 &&
         folder_name.compare(0, save_key.size(), save_key) == 0 &&
         folder_name[save_key.size()] == ' ';
}

std::string build_drive_rename_metadata_json(const std::string &name) {
  return "{\"name\":\"" + json_escape(name) + "\"}";
}

std::string build_drive_folder_metadata_json(const std::string &folder_name,
                                             const std::string &parent_id) {
  return "{\"name\":\"" + json_escape(folder_name) +
         "\",\"mimeType\":\"application/vnd.google-apps.folder\",\"parents\":[\"" +
         json_escape(parent_id) + "\"]}";
}

std::string build_drive_upload_metadata_json(const std::string &file_name,
                                             const std::string &parent_id) {
  return "{\"name\":\"" + json_escape(file_name) + "\",\"parents\":[\"" +
         json_escape(parent_id) + "\"]}";
}

std::string build_drive_sidecar_upload_metadata_json(const std::string &file_name,
                                                     const std::string &parent_id,
                                                     const std::string &archive_file_id) {
  // appProperties is private to Save Keeper and survives renaming either Drive file.
  return "{\"name\":\"" + json_escape(file_name) + "\",\"parents\":[\"" +
         json_escape(parent_id) + "\"],\"appProperties\":{\"archiveFileId\":\"" +
         json_escape(archive_file_id) + "\"}}";
}

std::string build_drive_sidecar_update_metadata_json(const std::string &file_name,
                                                     const std::string &archive_file_id) {
  // The file already has the correct parent. Omitting parents keeps this valid for Drive's update
  // endpoint while refreshing the stable archive link and canonical sidecar name.
  return "{\"name\":\"" + json_escape(file_name) +
         "\",\"appProperties\":{\"archiveFileId\":\"" +
         json_escape(archive_file_id) + "\"}}";
}

std::string build_drive_multipart_update_url(const std::string &file_id) {
  return "https://www.googleapis.com/upload/drive/v3/files/" + form_url_encode(file_id) +
         "?uploadType=multipart&fields=id%2Cname";
}

std::string build_drive_find_folder_query(const std::string &folder_name,
                                          const std::string &parent_id) {
  // Drive's query language has its own string escaping, then the whole q value is URL-encoded for
  // the GET request. Keeping these as separate steps prevents apostrophes in title names from
  // breaking the server-side query.
  const std::string query = "mimeType='application/vnd.google-apps.folder' and name='" +
                            drive_query_escape(folder_name) + "' and '" +
                            drive_query_escape(parent_id) + "' in parents and trashed=false";
  return "q=" + form_url_encode(query) + "&fields=files%28id%2Cname%29";
}

std::string build_drive_find_sidecar_by_archive_query(const std::string &parent_id,
                                                       const std::string &archive_file_id) {
  const std::string query = "'" + drive_query_escape(parent_id) +
                            "' in parents and appProperties has { key='archiveFileId' and "
                            "value='" +
                            drive_query_escape(archive_file_id) + "' } and trashed=false";
  return "q=" + form_url_encode(query) + "&fields=files%28id%2Cname%29";
}

std::string build_drive_find_child_by_name_query(const std::string &parent_id,
                                                  const std::string &name) {
  const std::string query = "name='" + drive_query_escape(name) + "' and '" +
                            drive_query_escape(parent_id) +
                            "' in parents and trashed=false";
  return "q=" + form_url_encode(query) + "&fields=files%28id%2Cname%29";
}

std::string build_drive_list_children_query(const std::string &parent_id) {
  const std::string query =
      "'" + drive_query_escape(parent_id) + "' in parents and trashed=false";
  return "q=" + form_url_encode(query) + "&fields=files%28id%2Cname%29";
}

namespace {

std::string build_paged_listing_query(const std::string &drive_query,
                                      const std::string &page_token) {
  std::string result = "q=" + form_url_encode(drive_query) +
                       "&fields=" + form_url_encode("nextPageToken,files(id,name,parents,size)") +
                       "&pageSize=1000";
  if (!page_token.empty()) {
    result += "&pageToken=" + form_url_encode(page_token);
  }
  return result;
}

} // namespace

std::string build_drive_list_all_folders_query(const std::string &page_token) {
  return build_paged_listing_query(
      "mimeType='application/vnd.google-apps.folder' and trashed=false", page_token);
}

std::string build_drive_list_all_files_query(const std::string &page_token) {
  // Narrow the server response to ZIP-like names, then retain the strict client-side suffix
  // check. Drive has no ends-with query operator, so "contains" is only a bandwidth filter.
  return build_paged_listing_query(
      "mimeType!='application/vnd.google-apps.folder' and name contains '.zip' and trashed=false",
      page_token);
}

namespace {

// Finds the closing brace of the object opened at open_pos, respecting string literals and
// nested containers. Returns npos when the object never closes.
std::size_t find_matching_brace(const std::string &json, std::size_t open_pos) {
  int depth = 0;
  bool in_string = false;
  for (std::size_t i = open_pos; i < json.size(); ++i) {
    const char ch = json[i];
    if (in_string) {
      if (ch == '\\') {
        ++i;
      } else if (ch == '"') {
        in_string = false;
      }
      continue;
    }
    if (ch == '"') {
      in_string = true;
    } else if (ch == '{' || ch == '[') {
      ++depth;
    } else if (ch == '}' || ch == ']') {
      --depth;
      if (depth == 0) {
        return i;
      }
    }
  }
  return std::string::npos;
}

// Drive returns object fields in no order the client may rely on; the live API currently emits
// "parents" before "id". Parsing each object as a bounded substring makes the field order
// irrelevant.
bool parse_drive_file_object(const std::string &object_json, DriveFile *file) {
  if (!find_json_string_from(object_json, 0, "id", &file->id, nullptr)) {
    return false;
  }
  find_json_string_from(object_json, 0, "name", &file->name, nullptr);
  // The parents value is an array of quoted ids; the string scanner right after the key picks up
  // the first array element.
  find_json_string_from(object_json, 0, "parents", &file->parent_id, nullptr);
  // int64 fields arrive as quoted strings in Drive v3 JSON.
  std::string size_text;
  if (find_json_string_from(object_json, 0, "size", &size_text, nullptr)) {
    const long long parsed = std::strtoll(size_text.c_str(), nullptr, 10);
    file->size_bytes = parsed > 0 ? parsed : 0;
  }
  return true;
}

} // namespace

DriveFileList parse_drive_file_list(const std::string &json) {
  DriveFileList result;
  find_json_string_from(json, 0, "nextPageToken", &result.next_page_token, nullptr);

  const std::size_t files_key = json.find("\"files\"");
  if (files_key == std::string::npos) {
    // Single-object responses (files.create, upload) carry the fields at the top level.
    DriveFile file;
    if (parse_drive_file_object(json, &file)) {
      result.files.push_back(std::move(file));
    }
    result.ok = true;
    return result;
  }

  const std::size_t array_start = json.find('[', files_key);
  if (array_start == std::string::npos) {
    result.error = "files array missing";
    return result;
  }

  std::size_t cursor = array_start + 1;
  while (cursor < json.size()) {
    const std::size_t object_start = json.find_first_of("{]", cursor);
    if (object_start == std::string::npos || json[object_start] == ']') {
      break;
    }
    const std::size_t object_end = find_matching_brace(json, object_start);
    if (object_end == std::string::npos) {
      result.error = "file entry not closed";
      return result;
    }
    DriveFile file;
    if (!parse_drive_file_object(
            json.substr(object_start, object_end - object_start + 1), &file)) {
      result.error = "file entry missing id";
      return result;
    }
    result.files.push_back(std::move(file));
    cursor = object_end + 1;
  }

  result.ok = true;
  return result;
}

} // namespace vsm
