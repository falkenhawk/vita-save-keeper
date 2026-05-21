#pragma once

#include <string>

namespace vsm {

struct GoogleClientCredentials {
  bool ok{};
  std::string client_id;
  std::string client_secret;
};

struct GoogleTokenCache {
  bool ok{};
  std::string access_token;
  std::string refresh_token;
  std::string token_type;
  long long expires_at_epoch_seconds{};
};

GoogleClientCredentials parse_google_client_credentials(const std::string &json);
std::string serialize_google_token_cache(const GoogleTokenCache &cache);
GoogleTokenCache parse_google_token_cache(const std::string &json);

} // namespace vsm
