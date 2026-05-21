#include "core/GoogleAuth.hpp"

#include <cctype>
#include <cstdlib>
#include <sstream>
#include <string>

namespace vsm {
namespace {

std::string form_pair(const std::string &key, const std::string &value) {
  return key + "=" + form_url_encode(value);
}

bool find_json_string(const std::string &json, const std::string &key, std::string *value) {
  const std::string needle = "\"" + key + "\"";
  const std::size_t key_pos = json.find(needle);
  if (key_pos == std::string::npos) {
    return false;
  }
  const std::size_t colon = json.find(':', key_pos + needle.size());
  if (colon == std::string::npos) {
    return false;
  }
  std::size_t cursor = colon + 1;
  while (cursor < json.size() && std::isspace(static_cast<unsigned char>(json[cursor]))) {
    ++cursor;
  }
  if (cursor >= json.size() || json[cursor] != '"') {
    return false;
  }
  ++cursor;

  std::string result;
  bool escaped = false;
  for (; cursor < json.size(); ++cursor) {
    const char ch = json[cursor];
    if (escaped) {
      switch (ch) {
      case '"':
      case '\\':
      case '/':
        result.push_back(ch);
        break;
      case 'n':
        result.push_back('\n');
        break;
      case 'r':
        result.push_back('\r');
        break;
      case 't':
        result.push_back('\t');
        break;
      default:
        result.push_back(ch);
        break;
      }
      escaped = false;
      continue;
    }
    if (ch == '\\') {
      escaped = true;
      continue;
    }
    if (ch == '"') {
      *value = result;
      return true;
    }
    result.push_back(ch);
  }

  return false;
}

bool find_json_int(const std::string &json, const std::string &key, int *value) {
  const std::string needle = "\"" + key + "\"";
  const std::size_t key_pos = json.find(needle);
  if (key_pos == std::string::npos) {
    return false;
  }
  const std::size_t colon = json.find(':', key_pos + needle.size());
  if (colon == std::string::npos) {
    return false;
  }
  const char *start = json.c_str() + colon + 1;
  char *end = nullptr;
  const long parsed = std::strtol(start, &end, 10);
  if (start == end) {
    return false;
  }
  *value = static_cast<int>(parsed);
  return true;
}

} // namespace

const char *const kGoogleDeviceCodeEndpoint = "https://oauth2.googleapis.com/device/code";
const char *const kGoogleTokenEndpoint = "https://oauth2.googleapis.com/token";
const char *const kGoogleDriveFileScope = "https://www.googleapis.com/auth/drive.file";
const char *const kGoogleDriveRootFolderName = "PSV Saves";

std::string form_url_encode(const std::string &value) {
  std::ostringstream out;
  out << std::uppercase << std::hex;
  for (const unsigned char ch : value) {
    if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
      out << static_cast<char>(ch);
    } else {
      // Google's examples encode spaces as %20. Using percent encoding instead of '+' keeps these
      // bodies valid both as application/x-www-form-urlencoded payloads and as easy-to-read test
      // fixtures when we compare exact request bodies.
      out << '%' << static_cast<const char *>("0123456789ABCDEF")[(ch >> 4U) & 0x0FU]
          << static_cast<const char *>("0123456789ABCDEF")[ch & 0x0FU];
    }
  }
  return out.str();
}

std::string build_device_code_request_body(const std::string &client_id) {
  return form_pair("client_id", client_id) + "&" + form_pair("scope", kGoogleDriveFileScope);
}

std::string build_device_token_request_body(const std::string &client_id,
                                            const std::string &client_secret,
                                            const std::string &device_code) {
  return form_pair("client_id", client_id) + "&" + form_pair("client_secret", client_secret) +
         "&" + form_pair("device_code", device_code) + "&" +
         form_pair("grant_type", "urn:ietf:params:oauth:grant-type:device_code");
}

std::string build_refresh_token_request_body(const std::string &client_id,
                                             const std::string &client_secret,
                                             const std::string &refresh_token) {
  std::string body = form_pair("client_id", client_id);
  if (!client_secret.empty()) {
    body += "&" + form_pair("client_secret", client_secret);
  }
  body += "&" + form_pair("refresh_token", refresh_token) + "&" +
          form_pair("grant_type", "refresh_token");
  return body;
}

DeviceCodeResponse parse_device_code_response(const std::string &json) {
  DeviceCodeResponse response;
  find_json_string(json, "error", &response.error);
  find_json_string(json, "error_description", &response.error_description);
  find_json_string(json, "device_code", &response.device_code);
  find_json_string(json, "user_code", &response.user_code);
  find_json_string(json, "verification_url", &response.verification_url);
  find_json_int(json, "expires_in", &response.expires_in);
  find_json_int(json, "interval", &response.interval);
  response.ok = response.error.empty() && !response.device_code.empty() &&
                !response.user_code.empty() && !response.verification_url.empty();
  return response;
}

TokenResponse parse_token_response(const std::string &json) {
  TokenResponse response;
  find_json_string(json, "error", &response.error);
  find_json_string(json, "error_description", &response.error_description);
  find_json_string(json, "access_token", &response.access_token);
  find_json_string(json, "refresh_token", &response.refresh_token);
  find_json_string(json, "scope", &response.scope);
  find_json_string(json, "token_type", &response.token_type);
  find_json_int(json, "expires_in", &response.expires_in);
  response.ok = response.error.empty() && !response.access_token.empty();
  return response;
}

} // namespace vsm
