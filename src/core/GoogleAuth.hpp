#pragma once

#include <string>

namespace vsm {

extern const char *const kGoogleDeviceCodeEndpoint;
extern const char *const kGoogleTokenEndpoint;
extern const char *const kGoogleDriveFileScope;
extern const char *const kGoogleDriveRootFolderName;

struct DeviceCodeResponse {
  bool ok{};
  std::string device_code;
  std::string user_code;
  std::string verification_url;
  int expires_in{};
  int interval{};
  std::string error;
  std::string error_description;
};

struct TokenResponse {
  bool ok{};
  std::string access_token;
  std::string refresh_token;
  std::string scope;
  std::string token_type;
  int expires_in{};
  std::string error;
  std::string error_description;
};

std::string form_url_encode(const std::string &value);
std::string build_device_code_request_body(const std::string &client_id);
std::string build_device_token_request_body(const std::string &client_id,
                                            const std::string &client_secret,
                                            const std::string &device_code);
std::string build_refresh_token_request_body(const std::string &client_id,
                                             const std::string &client_secret,
                                             const std::string &refresh_token);
DeviceCodeResponse parse_device_code_response(const std::string &json);
TokenResponse parse_token_response(const std::string &json);

} // namespace vsm
