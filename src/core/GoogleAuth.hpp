#pragma once

#include <string>

namespace vsm {

extern const char *const kGoogleDeviceCodeEndpoint;
extern const char *const kGoogleTokenEndpoint;
extern const char *const kGoogleDriveFileScope;
extern const char *const kGoogleDriveRootFolderName;

std::string form_url_encode(const std::string &value);
std::string build_device_code_request_body(const std::string &client_id);
std::string build_device_token_request_body(const std::string &client_id,
                                            const std::string &client_secret,
                                            const std::string &device_code);
std::string build_refresh_token_request_body(const std::string &client_id,
                                             const std::string &client_secret,
                                             const std::string &refresh_token);

} // namespace vsm
