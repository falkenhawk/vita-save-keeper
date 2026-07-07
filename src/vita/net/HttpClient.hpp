#pragma once

#include <functional>
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
  // Called during transfers so the UI can draw progress frames. total <= 0 means the size is
  // unknown and the UI should show an indeterminate busy state.
  using ProgressHook =
      std::function<void(const std::string &label, long long done, long long total)>;

  // Initializes the Sony network stack and libcurl once for the whole app run. Re-initializing
  // per request (the previous design) breaks when the stack is already up: sceNetInit fails and
  // every request afterwards reported "curl init failed". Safe to call repeatedly.
  static bool network_startup(std::string *error_message);
  static void network_shutdown();
  // Live connectivity, not configuration: true only while the console actually has an internet
  // connection. Cheap enough to poll every second.
  static bool network_reachable();

  static void set_progress_hook(ProgressHook hook);
  // Polled during transfers (at the progress-frame interval); returning true aborts the request,
  // which surfaces as a failed HttpResponse. Lets a batch cancel mid-upload instead of waiting
  // out the transfer.
  using CancelCheck = std::function<bool()>;
  static void set_cancel_check(CancelCheck check);
  // Requests report progress only while a busy label is set; label-less requests (such as the
  // background sign-in polls) stay silent instead of flashing a modal every few seconds.
  static void set_busy_label(std::string label);
  // When false, the progress callback ignores download byte counts and reports an indeterminate
  // state instead. The batch sets this so a Drive folder lookup/create response (a quick download)
  // does not flash the per-file percent to 100% right before the actual upload counts up.
  static void set_report_downloads(bool report);
  static const std::string &busy_label();

  HttpResponse post_form(const std::string &url, const std::string &body) const;
  HttpResponse get_json(const std::string &url, const std::string &bearer_token) const;
  HttpResponse post_json(const std::string &url, const std::string &json,
                         const std::string &bearer_token) const;
  HttpResponse patch_json(const std::string &url, const std::string &json,
                          const std::string &bearer_token) const;
  HttpResponse post_multipart_file(const std::string &url, const std::string &metadata_json,
                                   const std::string &file_path,
                                   const std::string &bearer_token) const;
  HttpResponse download_file(const std::string &url, const std::string &file_path,
                             const std::string &bearer_token) const;
  HttpResponse delete_request(const std::string &url, const std::string &bearer_token) const;
};

// Sets the busy label for the enclosing operation and restores the previous one on scope exit,
// so nested helpers (token refresh inside an upload) keep the outer label.
class BusyLabelScope {
public:
  explicit BusyLabelScope(const char *label);
  ~BusyLabelScope();

  BusyLabelScope(const BusyLabelScope &) = delete;
  BusyLabelScope &operator=(const BusyLabelScope &) = delete;

private:
  std::string previous_;
};

} // namespace vsm::vita
