#include "vita/net/HttpClient.hpp"

#include <curl/curl.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/net/http.h>
#include <psp2/sysmodule.h>
#include <cstdlib>
#include <cstdio>

namespace vsm::vita {
namespace {

std::size_t append_response_body(char *ptr, std::size_t size, std::size_t nmemb, void *userdata) {
  const std::size_t byte_count = size * nmemb;
  auto *body = static_cast<std::string *>(userdata);
  body->append(ptr, byte_count);
  return byte_count;
}

HttpResponse curl_error(const char *message) {
  HttpResponse response;
  response.error = message ? message : "curl error";
  return response;
}

curl_slist *append_bearer_header(curl_slist *headers, const std::string &bearer_token) {
  if (bearer_token.empty()) {
    return headers;
  }
  return curl_slist_append(headers, ("Authorization: Bearer " + bearer_token).c_str());
}

HttpResponse perform_with_body(CURL *curl, curl_slist *headers) {
  HttpResponse response;
  char error_buffer[CURL_ERROR_SIZE] {};
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_response_body);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
  curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);

  const CURLcode code = curl_easy_perform(curl);
  if (code == CURLE_OK) {
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
    response.ok = response.status >= 200 && response.status < 300;
  } else {
    response.error = error_buffer[0] != '\0' ? error_buffer : curl_easy_strerror(code);
  }
  return response;
}

std::size_t write_file_body(char *ptr, std::size_t size, std::size_t nmemb, void *userdata) {
  return std::fwrite(ptr, size, nmemb, static_cast<FILE *>(userdata));
}

bool read_binary_file(const std::string &path, std::string *contents) {
  FILE *file = std::fopen(path.c_str(), "rb");
  if (!file) {
    return false;
  }

  contents->clear();
  char buffer[32 * 1024];
  while (true) {
    const std::size_t read = std::fread(buffer, 1, sizeof(buffer), file);
    if (read > 0) {
      contents->append(buffer, read);
    }
    if (read < sizeof(buffer)) {
      const bool ok = std::ferror(file) == 0;
      std::fclose(file);
      return ok;
    }
  }
}

} // namespace

HttpClient::HttpClient() {
  constexpr int kNetMemorySize = 4 * 1024 * 1024;
  net_module_loaded_ = sceSysmoduleLoadModule(SCE_SYSMODULE_NET) >= 0;

  net_memory_ = std::malloc(kNetMemorySize);
  if (!net_memory_) {
    return;
  }

  SceNetInitParam net_init_param {};
  net_init_param.memory = net_memory_;
  net_init_param.size = kNetMemorySize;
  net_init_param.flags = 0;
  if (sceNetInit(&net_init_param) < 0) {
    return;
  }
  net_initialized_ = true;

  if (sceNetCtlInit() < 0) {
    return;
  }
  netctl_initialized_ = true;

  http_module_loaded_ = sceSysmoduleLoadModule(SCE_SYSMODULE_HTTP) >= 0;
  if (sceHttpInit(kNetMemorySize) < 0) {
    return;
  }
  http_initialized_ = true;

  // VitaSDK's libcurl sample initializes the Vita NET and HTTP modules before curl. Keep that
  // ordering here so DNS, TLS, and HTTP share Sony's initialized networking stack on hardware.
  initialized_ = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
}

HttpClient::~HttpClient() {
  if (initialized_) {
    curl_global_cleanup();
  }
  if (http_initialized_) {
    sceHttpTerm();
  }
  if (http_module_loaded_) {
    sceSysmoduleUnloadModule(SCE_SYSMODULE_HTTP);
  }
  if (netctl_initialized_) {
    sceNetCtlTerm();
  }
  if (net_initialized_) {
    sceNetTerm();
  }
  if (net_module_loaded_) {
    sceSysmoduleUnloadModule(SCE_SYSMODULE_NET);
  }
  std::free(net_memory_);
}

HttpResponse HttpClient::post_form(const std::string &url, const std::string &body) const {
  if (!initialized_) {
    return curl_error("curl init failed");
  }

  CURL *curl = curl_easy_init();
  if (!curl) {
    return curl_error("curl easy init failed");
  }

  curl_slist *headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");
  headers = curl_slist_append(headers, "Accept: application/json");

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));

  // Do not disable TLS verification here. OAuth refresh tokens are long-lived enough that accepting
  // any certificate would be a real account risk; if Vita certs are insufficient, the app should
  // ship or configure a CA bundle instead of silently weakening HTTPS.
  const HttpResponse response = perform_with_body(curl, headers);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  return response;
}

HttpResponse HttpClient::get_json(const std::string &url, const std::string &bearer_token) const {
  if (!initialized_) {
    return curl_error("curl init failed");
  }

  CURL *curl = curl_easy_init();
  if (!curl) {
    return curl_error("curl easy init failed");
  }

  curl_slist *headers = nullptr;
  headers = curl_slist_append(headers, "Accept: application/json");
  headers = append_bearer_header(headers, bearer_token);
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());

  const HttpResponse response = perform_with_body(curl, headers);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  return response;
}

HttpResponse HttpClient::post_json(const std::string &url, const std::string &json,
                                   const std::string &bearer_token) const {
  if (!initialized_) {
    return curl_error("curl init failed");
  }

  CURL *curl = curl_easy_init();
  if (!curl) {
    return curl_error("curl easy init failed");
  }

  curl_slist *headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  headers = curl_slist_append(headers, "Accept: application/json");
  headers = append_bearer_header(headers, bearer_token);
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(json.size()));

  const HttpResponse response = perform_with_body(curl, headers);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  return response;
}

HttpResponse HttpClient::post_multipart_file(const std::string &url,
                                             const std::string &metadata_json,
                                             const std::string &file_path,
                                             const std::string &bearer_token) const {
  if (!initialized_) {
    return curl_error("curl init failed");
  }

  CURL *curl = curl_easy_init();
  if (!curl) {
    return curl_error("curl easy init failed");
  }

  curl_slist *headers = nullptr;
  headers = curl_slist_append(headers, "Accept: application/json");
  headers = append_bearer_header(headers, bearer_token);

  std::string file_contents;
  if (!read_binary_file(file_path, &file_contents)) {
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return curl_error("could not read upload file");
  }

  // Drive's multipart upload endpoint expects multipart/related, while libcurl's MIME helper emits
  // multipart/form-data. Build the small snapshot upload body directly for now; larger saves can
  // move to Drive's resumable upload flow when we add progress reporting.
  constexpr const char *kBoundary = "save-keeper-drive-boundary";
  const std::string body = std::string("--") + kBoundary +
                           "\r\nContent-Type: application/json; charset=UTF-8\r\n\r\n" +
                           metadata_json + "\r\n--" + kBoundary +
                           "\r\nContent-Type: application/zip\r\n\r\n" + file_contents +
                           "\r\n--" + kBoundary + "--\r\n";
  headers = curl_slist_append(
      headers, (std::string("Content-Type: multipart/related; boundary=") + kBoundary).c_str());

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));

  const HttpResponse response = perform_with_body(curl, headers);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  return response;
}

HttpResponse HttpClient::download_file(const std::string &url, const std::string &file_path,
                                       const std::string &bearer_token) const {
  if (!initialized_) {
    return curl_error("curl init failed");
  }

  FILE *output = std::fopen(file_path.c_str(), "wb");
  if (!output) {
    return curl_error("could not open download file");
  }

  CURL *curl = curl_easy_init();
  if (!curl) {
    std::fclose(output);
    return curl_error("curl easy init failed");
  }

  HttpResponse response;
  char error_buffer[CURL_ERROR_SIZE] {};
  curl_slist *headers = nullptr;
  headers = append_bearer_header(headers, bearer_token);

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_file_body);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, output);
  curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);

  const CURLcode code = curl_easy_perform(curl);
  if (code == CURLE_OK) {
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
    response.ok = response.status >= 200 && response.status < 300;
  } else {
    response.error = error_buffer[0] != '\0' ? error_buffer : curl_easy_strerror(code);
  }

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  if (std::fclose(output) != 0) {
    response.ok = false;
    response.error = "could not close download file";
  }
  return response;
}

} // namespace vsm::vita
