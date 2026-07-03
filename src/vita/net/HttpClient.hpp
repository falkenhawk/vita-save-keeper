#pragma once

#include <string>

namespace vsm::vita {

struct HttpResponse {
  bool ok{};
  long status{};
  std::string body;
  std::string error;
};

class HttpClient {
public:
  // Initializes the Sony network stack and libcurl once for the whole app run. Re-initializing
  // per request (the previous design) breaks when the stack is already up: sceNetInit fails and
  // every request afterwards reported "curl init failed". Safe to call repeatedly.
  static bool network_startup(std::string *error_message);
  static void network_shutdown();

  HttpResponse post_form(const std::string &url, const std::string &body) const;
  HttpResponse get_json(const std::string &url, const std::string &bearer_token) const;
  HttpResponse post_json(const std::string &url, const std::string &json,
                         const std::string &bearer_token) const;
  HttpResponse post_multipart_file(const std::string &url, const std::string &metadata_json,
                                   const std::string &file_path,
                                   const std::string &bearer_token) const;
  HttpResponse download_file(const std::string &url, const std::string &file_path,
                             const std::string &bearer_token) const;
};

} // namespace vsm::vita
