#include "core/GoogleConfig.hpp"

#include "core/GoogleAuth.hpp"

#include <cstdlib>
#include <sstream>
#include <string>

#ifndef SAVE_KEEPER_GOOGLE_CLIENT_ID
#define SAVE_KEEPER_GOOGLE_CLIENT_ID ""
#endif

#ifndef SAVE_KEEPER_GOOGLE_CLIENT_SECRET
#define SAVE_KEEPER_GOOGLE_CLIENT_SECRET ""
#endif

namespace vsm {
namespace {

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
      return true;
    }
    result.push_back(ch);
  }

  return false;
}

bool find_json_long_long(const std::string &json, const std::string &key, long long *value) {
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
  const long long parsed = std::strtoll(start, &end, 10);
  if (start == end) {
    return false;
  }
  *value = parsed;
  return true;
}

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

} // namespace

GoogleClientCredentials parse_google_client_credentials(const std::string &json) {
  GoogleClientCredentials credentials;
  find_json_string(json, "client_id", &credentials.client_id);
  find_json_string(json, "client_secret", &credentials.client_secret);
  credentials.ok = !credentials.client_id.empty() && !credentials.client_secret.empty();
  return credentials;
}

GoogleClientCredentials embedded_google_client_credentials() {
  GoogleClientCredentials credentials;
  credentials.client_id = SAVE_KEEPER_GOOGLE_CLIENT_ID;
  credentials.client_secret = SAVE_KEEPER_GOOGLE_CLIENT_SECRET;
  credentials.ok = !credentials.client_id.empty() && !credentials.client_secret.empty();
  return credentials;
}

std::string serialize_google_token_cache(const GoogleTokenCache &cache) {
  std::ostringstream out;
  out << "{\"access_token\":\"" << json_escape(cache.access_token) << "\","
      << "\"refresh_token\":\"" << json_escape(cache.refresh_token) << "\","
      << "\"token_type\":\"" << json_escape(cache.token_type) << "\","
      << "\"expires_at_epoch_seconds\":" << cache.expires_at_epoch_seconds << "}";
  return out.str();
}

GoogleTokenCache parse_google_token_cache(const std::string &json) {
  GoogleTokenCache cache;
  find_json_string(json, "access_token", &cache.access_token);
  find_json_string(json, "refresh_token", &cache.refresh_token);
  find_json_string(json, "token_type", &cache.token_type);
  find_json_long_long(json, "expires_at_epoch_seconds", &cache.expires_at_epoch_seconds);
  cache.ok = !cache.refresh_token.empty();
  return cache;
}

} // namespace vsm
