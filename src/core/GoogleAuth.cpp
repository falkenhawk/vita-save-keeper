#include "core/GoogleAuth.hpp"

#include <cctype>
#include <sstream>

namespace vsm {
namespace {

std::string form_pair(const std::string &key, const std::string &value) {
  return key + "=" + form_url_encode(value);
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

} // namespace vsm
