#include "core/DiagTrace.hpp"

#include <cstdio>

namespace vsm {
namespace {

// empty path means disabled; there is exactly one trace per run so a single global is enough
std::string g_trace_path;

bool append_line(const std::string &line) {
  FILE *file = std::fopen(g_trace_path.c_str(), "ab");
  if (!file) {
    return false;
  }
  std::fwrite(line.data(), 1, line.size(), file);
  std::fputc('\n', file);
  // fclose flushes to the filesystem; that commit per line is what makes the trace readable
  // after the forced power-off that ends a hung boot
  std::fclose(file);
  return true;
}

} // namespace

bool diag_enabled() {
  return !g_trace_path.empty();
}

bool diag_open(const std::string &path, const std::string &header) {
  g_trace_path.clear();
  FILE *file = std::fopen(path.c_str(), "wb");
  if (!file) {
    return false;
  }
  std::fclose(file);
  g_trace_path = path;
  diag_log(header);
  return true;
}

void diag_log(const std::string &line) {
  if (g_trace_path.empty()) {
    return;
  }
  append_line(line);
}

void diag_close(const std::string &footer) {
  diag_log(footer);
  g_trace_path.clear();
}

bool diag_should_log_count(long long count) {
  if (count < 1000) {
    return false;
  }
  long long threshold = 1000;
  while (threshold < count) {
    threshold *= 2;
  }
  return threshold == count;
}

std::string diag_safe(const std::string &text) {
  std::string result = text;
  for (char &ch : result) {
    if (static_cast<unsigned char>(ch) < 0x20) {
      ch = ' ';
    }
  }
  return result;
}

} // namespace vsm
