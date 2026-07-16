#include "vita/net/HttpClient.hpp"

#include <curl/curl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>
#include <cstdio>
#include <cstdlib>
#include <utility>

namespace vsm::vita {
namespace {

// VitaSDK's libcurl+OpenSSL has no default CA store on Vita, so TLS verification fails for every
// HTTPS request unless a CA bundle is provided explicitly. The Mozilla bundle is packaged in the
// VPK; keeping verification on protects the long-lived OAuth refresh token.
constexpr const char *kCaBundlePath = "app0:sce_sys/resources/cacert.pem";
constexpr int kNetMemorySize = 1024 * 1024;
// Progress frames are throttled so slow vita2d redraws do not starve the actual transfer.
constexpr SceUInt64 kProgressFrameIntervalUs = 100 * 1000;

bool g_network_ready = false;
void *g_net_memory = nullptr;
// One reused easy handle for the whole app. curl_easy_reset clears request options but keeps the
// connection and TLS session caches, so repeated Google calls skip DNS, TCP, and the handshake.
// That reuse is what makes sign-in polling and Drive round-trips fast enough on the Vita CPU.
CURL *g_curl = nullptr;
HttpClient::ProgressHook g_progress_hook;
HttpClient::CancelCheck g_cancel_check;
std::string g_busy_label;
bool g_report_downloads = true;
SceUInt64 g_last_progress_us = 0;

std::size_t append_response_body(char *ptr, std::size_t size, std::size_t nmemb, void *userdata) {
  const std::size_t byte_count = size * nmemb;
  auto *body = static_cast<std::string *>(userdata);
  body->append(ptr, byte_count);
  return byte_count;
}

HttpResponse transport_error(const char *message) {
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

void emit_progress(long long done, long long total) {
  if (!g_progress_hook || g_busy_label.empty()) {
    return;
  }
  g_progress_hook(g_busy_label, done, total);
}

int transfer_progress(void *, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal,
                      curl_off_t ulnow) {
  const SceUInt64 now = sceKernelGetProcessTimeWide();
  if (now - g_last_progress_us < kProgressFrameIntervalUs) {
    return 0;
  }
  g_last_progress_us = now;

  if (g_cancel_check && g_cancel_check()) {
    // Non-zero return makes curl abort with CURLE_ABORTED_BY_CALLBACK.
    return 1;
  }

  if (ultotal > 0) {
    emit_progress(ulnow, ultotal);
  } else if (dltotal > 0 && g_report_downloads) {
    emit_progress(dlnow, dltotal);
  } else {
    emit_progress(0, -1);
  }
  return 0;
}

CURL *acquire_handle() {
  if (!g_network_ready || !g_curl) {
    return nullptr;
  }
  curl_easy_reset(g_curl);
  return g_curl;
}

void configure_common(CURL *curl, curl_slist *headers, char *error_buffer, long timeout_seconds) {
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_seconds);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_CAINFO, kCaBundlePath);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "save-keeper/1.0 (PS Vita)");

  if (g_progress_hook && !g_busy_label.empty()) {
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, transfer_progress);
    // Draw one busy frame before the transfer starts, so even DNS and the TLS handshake happen
    // behind a visible "working" screen instead of a frozen UI.
    emit_progress(0, -1);
  }
}

HttpResponse perform_with_body(CURL *curl, curl_slist *headers, long timeout_seconds = 120L) {
  HttpResponse response;
  char error_buffer[CURL_ERROR_SIZE] {};
  configure_common(curl, headers, error_buffer, timeout_seconds);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_response_body);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);

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

HttpResponse send_multipart_file(const std::string &url, const std::string &metadata_json,
                                 const std::string &file_path,
                                 const std::string &bearer_token, bool update_existing) {
  CURL *curl = acquire_handle();
  if (!curl) {
    return transport_error("network not initialized");
  }

  curl_slist *headers = nullptr;
  headers = curl_slist_append(headers, "Accept: application/json");
  headers = append_bearer_header(headers, bearer_token);

  std::string file_contents;
  if (!read_binary_file(file_path, &file_contents)) {
    curl_slist_free_all(headers);
    return transport_error("could not read upload file");
  }

  // Drive expects multipart/related, while libcurl's MIME helper emits multipart/form-data.
  constexpr const char *kBoundary = "save-keeper-drive-boundary";
  const std::string body = std::string("--") + kBoundary +
                           "\r\nContent-Type: application/json; charset=UTF-8\r\n\r\n" +
                           metadata_json + "\r\n--" + kBoundary +
                           "\r\nContent-Type: application/zip\r\n\r\n" + file_contents +
                           "\r\n--" + kBoundary + "--\r\n";
  headers = curl_slist_append(
      headers, (std::string("Content-Type: multipart/related; boundary=") + kBoundary).c_str());

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  if (update_existing) {
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PATCH");
  } else {
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
  }
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));

  const HttpResponse response = perform_with_body(curl, headers);
  curl_slist_free_all(headers);
  return response;
}

} // namespace

bool HttpClient::network_startup(std::string *error_message) {
  if (g_network_ready) {
    return true;
  }

  if (sceSysmoduleIsLoaded(SCE_SYSMODULE_NET) != SCE_SYSMODULE_LOADED &&
      sceSysmoduleLoadModule(SCE_SYSMODULE_NET) < 0) {
    if (error_message) {
      *error_message = "Could not load the network module.";
    }
    return false;
  }

  // sceNetShowNetstat returns 0 only when the net library is already initialized (for example by
  // the system or a previous run that did not terminate it). Initializing twice fails, so only
  // call sceNetInit when the stack is actually down.
  if (sceNetShowNetstat() != 0) {
    g_net_memory = std::malloc(kNetMemorySize);
    if (!g_net_memory) {
      if (error_message) {
        *error_message = "Out of memory for networking.";
      }
      return false;
    }
    SceNetInitParam net_init_param {};
    net_init_param.memory = g_net_memory;
    net_init_param.size = kNetMemorySize;
    net_init_param.flags = 0;
    if (sceNetInit(&net_init_param) < 0) {
      if (error_message) {
        *error_message = "Network stack initialization failed.";
      }
      std::free(g_net_memory);
      g_net_memory = nullptr;
      return false;
    }
  }

  // sceNetCtlInit fails when already initialized; that state is fine for making requests, so the
  // result is intentionally ignored.
  sceNetCtlInit();

  if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
    if (error_message) {
      *error_message = "curl initialization failed.";
    }
    return false;
  }

  g_curl = curl_easy_init();
  if (!g_curl) {
    if (error_message) {
      *error_message = "curl handle allocation failed.";
    }
    curl_global_cleanup();
    return false;
  }

  g_network_ready = true;
  return true;
}

bool HttpClient::network_reachable() {
  int state = 0;
  if (sceNetCtlInetGetState(&state) < 0) {
    return false;
  }
  return state == SCE_NETCTL_STATE_CONNECTED;
}

void HttpClient::network_shutdown() {
  if (!g_network_ready) {
    return;
  }
  if (g_curl) {
    curl_easy_cleanup(g_curl);
    g_curl = nullptr;
  }
  curl_global_cleanup();
  // The Sony net stack is intentionally left running: sceNetTerm on a stack the system also uses
  // can break other consumers, and process exit reclaims everything anyway.
  g_network_ready = false;
}

void HttpClient::set_progress_hook(ProgressHook hook) {
  g_progress_hook = std::move(hook);
}

void HttpClient::set_cancel_check(CancelCheck check) {
  g_cancel_check = std::move(check);
}

void HttpClient::set_busy_label(std::string label) {
  g_busy_label = std::move(label);
}

const std::string &HttpClient::busy_label() {
  return g_busy_label;
}

void HttpClient::set_report_downloads(bool report) {
  g_report_downloads = report;
}

BusyLabelScope::BusyLabelScope(const char *label) : previous_(HttpClient::busy_label()) {
  HttpClient::set_busy_label(label ? label : "");
  // Draw the modal once, immediately, so it appears the instant the operation starts rather than
  // only when the transfer's first progress callback fires. A large upload spends a noticeable
  // beat first reading the file and packing the multipart body, during which curl reports nothing;
  // without this the screen sits frozen on the previous frame. total < 0 = indeterminate sweep.
  if (g_progress_hook && !g_busy_label.empty()) {
    g_progress_hook(g_busy_label, 0, -1);
  }
}

BusyLabelScope::~BusyLabelScope() {
  HttpClient::set_busy_label(previous_);
}

HttpResponse HttpClient::post_form(const std::string &url, const std::string &body) const {
  CURL *curl = acquire_handle();
  if (!curl) {
    return transport_error("network not initialized");
  }

  curl_slist *headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");
  headers = curl_slist_append(headers, "Accept: application/json");

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));

  const HttpResponse response = perform_with_body(curl, headers);
  curl_slist_free_all(headers);
  return response;
}

HttpResponse HttpClient::get_json(const std::string &url, const std::string &bearer_token) const {
  CURL *curl = acquire_handle();
  if (!curl) {
    return transport_error("network not initialized");
  }

  curl_slist *headers = nullptr;
  headers = curl_slist_append(headers, "Accept: application/json");
  headers = append_bearer_header(headers, bearer_token);
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());

  const HttpResponse response = perform_with_body(curl, headers);
  curl_slist_free_all(headers);
  return response;
}

HttpResponse HttpClient::post_json(const std::string &url, const std::string &json,
                                   const std::string &bearer_token) const {
  CURL *curl = acquire_handle();
  if (!curl) {
    return transport_error("network not initialized");
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
  return response;
}

HttpResponse HttpClient::patch_json(const std::string &url, const std::string &json,
                                    const std::string &bearer_token) const {
  CURL *curl = acquire_handle();
  if (!curl) {
    return transport_error("network not initialized");
  }

  curl_slist *headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  headers = curl_slist_append(headers, "Accept: application/json");
  headers = append_bearer_header(headers, bearer_token);
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PATCH");
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(json.size()));

  const HttpResponse response = perform_with_body(curl, headers);
  curl_slist_free_all(headers);
  return response;
}

HttpResponse HttpClient::post_multipart_file(const std::string &url,
                                             const std::string &metadata_json,
                                             const std::string &file_path,
                                             const std::string &bearer_token) const {
  return send_multipart_file(url, metadata_json, file_path, bearer_token, false);
}

HttpResponse HttpClient::patch_multipart_file(const std::string &url,
                                              const std::string &metadata_json,
                                              const std::string &file_path,
                                              const std::string &bearer_token) const {
  return send_multipart_file(url, metadata_json, file_path, bearer_token, true);
}

HttpResponse HttpClient::download_file(const std::string &url, const std::string &file_path,
                                       const std::string &bearer_token) const {
  CURL *curl = acquire_handle();
  if (!curl) {
    return transport_error("network not initialized");
  }

  FILE *output = std::fopen(file_path.c_str(), "wb");
  if (!output) {
    return transport_error("could not open download file");
  }

  HttpResponse response;
  char error_buffer[CURL_ERROR_SIZE] {};
  curl_slist *headers = nullptr;
  headers = append_bearer_header(headers, bearer_token);

  configure_common(curl, headers, error_buffer, 300L);
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_file_body);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, output);

  const CURLcode code = curl_easy_perform(curl);
  if (code == CURLE_OK) {
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
    response.ok = response.status >= 200 && response.status < 300;
  } else {
    response.error = error_buffer[0] != '\0' ? error_buffer : curl_easy_strerror(code);
  }

  curl_slist_free_all(headers);
  if (std::fclose(output) != 0) {
    response.ok = false;
    response.error = "could not close download file";
  }
  return response;
}

HttpResponse HttpClient::delete_request(const std::string &url,
                                        const std::string &bearer_token) const {
  CURL *curl = acquire_handle();
  if (!curl) {
    return transport_error("network not initialized");
  }

  curl_slist *headers = nullptr;
  headers = curl_slist_append(headers, "Accept: application/json");
  headers = append_bearer_header(headers, bearer_token);
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");

  const HttpResponse response = perform_with_body(curl, headers);
  curl_slist_free_all(headers);
  return response;
}

} // namespace vsm::vita
