#pragma once

#include <string>

namespace vsm {

std::string normalize_path_component(const std::string &input);

// Joins a parent directory and a child name with a single '/'. An empty parent yields the child
// unchanged, and an existing trailing '/' is not doubled.
std::string join_path(const std::string &parent, const std::string &child);

} // namespace vsm
