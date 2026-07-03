#include "core/GoogleDrive.hpp"

#include "core/GoogleAuth.hpp"

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

std::string build_drive_list_children_query(const std::string &parent_id) {
  const std::string query =
      "'" + drive_query_escape(parent_id) + "' in parents and trashed=false";
  return "q=" + form_url_encode(query) + "&fields=files%28id%2Cname%29";
}

namespace {

std::string build_paged_listing_query(const std::string &drive_query,
                                      const std::string &page_token) {
  std::string result = "q=" + form_url_encode(drive_query) +
                       "&fields=" + form_url_encode("nextPageToken,files(id,name,parents)") +
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
  // Everything that is not a folder: backups are matched by their .zip name afterwards, which is
  // more reliable than trusting the mime type Drive assigned at upload time.
  return build_paged_listing_query(
      "mimeType!='application/vnd.google-apps.folder' and trashed=false", page_token);
}

DriveFileList parse_drive_file_list(const std::string &json) {
  DriveFileList result;
  find_json_string_from(json, 0, "nextPageToken", &result.next_page_token, nullptr);

  std::size_t cursor = 0;
  while (true) {
    std::string id;
    std::size_t id_end = 0;
    if (!find_json_string_from(json, cursor, "id", &id, &id_end)) {
      break;
    }

    std::string name;
    std::size_t name_end = 0;
    if (!find_json_string_from(json, id_end, "name", &name, &name_end)) {
      result.error = "file entry missing name";
      return result;
    }
    cursor = name_end;

    // Drive lists "parents" after "name" inside each file object. Only accept a parents key that
    // appears before the next file's "id", otherwise this entry has none and the key belongs to a
    // later file.
    std::string parent_id;
    const std::size_t next_id = json.find("\"id\"", name_end);
    const std::size_t parents_pos = json.find("\"parents\"", name_end);
    if (parents_pos != std::string::npos &&
        (next_id == std::string::npos || parents_pos < next_id)) {
      std::size_t parent_end = 0;
      // The parents value is an array of quoted ids; reusing the string scanner on the region
      // right after the key picks up the first array element.
      if (find_json_string_from(json, parents_pos, "parents", &parent_id, &parent_end)) {
        cursor = parent_end;
      }
    }

    result.files.push_back({id, name, parent_id});
  }

  result.ok = true;
  return result;
}

} // namespace vsm
