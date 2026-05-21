#pragma once

#include <string>
#include <vector>

namespace vsm::vita {

struct HttpResponse {
  bool ok{};
  long status{};
  std::string body;
  std::string error;
};

class HttpClient {
public:
  HttpClient();
  ~HttpClient();

  HttpClient(const HttpClient &) = delete;
  HttpClient &operator=(const HttpClient &) = delete;

  HttpResponse post_form(const std::string &url, const std::string &body) const;

private:
  bool initialized_{};
};

} // namespace vsm::vita
