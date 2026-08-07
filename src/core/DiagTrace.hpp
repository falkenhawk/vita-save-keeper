#pragma once

#include <string>

namespace vsm {

// Boot-trace logging for diagnostic builds (github issue #7: startup hang mid-scan). Every call
// is a no-op until diag_open succeeds, and diag_close disables logging again, so a trace covers
// exactly the startup sequence and instrumented code paths cost nothing in normal operation.
//
// Each line is written with its own open-append-close cycle. That is deliberately slow: the
// reporter's console hangs and gets powered off by force, so every line must already be on the
// card when that happens. The last line in the log names the operation that never returned.
// Lines carry an elapsed-seconds prefix ("12.345 ") counted from diag_open, so a completed
// trace also shows how long every stage took.
bool diag_enabled();
bool diag_open(const std::string &path, const std::string &header);
void diag_log(const std::string &line);
void diag_close(const std::string &footer);

// Sparse schedule for progress lines inside directory walks: true at 1000, 2000, 4000, ...
// entries. A healthy directory never reaches the first threshold, while a corrupt FAT chain
// that makes readdir loop forever proves itself in the log at a rate that cannot fill the card.
bool diag_should_log_count(long long count);

// A control byte in a logged value (an sfo title is arbitrary bytes, not trusted text) would
// break the one-line-per-event format the log is read by; replace them with spaces.
std::string diag_safe(const std::string &text);

} // namespace vsm
