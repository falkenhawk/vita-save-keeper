#pragma once

#include <string>

namespace vsm {

std::string normalize_path_component(const std::string &input);

// Folder-safe form of a game title, built to be read rather than reversed: the characters a
// filesystem or Drive refuses are resolved at the source instead of being masked as '_', so the
// folder name is the title. Quotes and apostrophes are dropped because they sit inside a word
// ("Everybody's" reads as "Everybodys"); every other unsafe character becomes a space because it
// separates words ("GTA: Liberty City" reads as "GTA Liberty City"); then whitespace runs
// collapse and the ends are trimmed. A title carrying none of them comes back unchanged.
std::string normalize_folder_title(const std::string &title);

// Joins a parent directory and a child name with a single '/'. An empty parent yields the child
// unchanged, and an existing trailing '/' is not doubled.
std::string join_path(const std::string &parent, const std::string &child);

} // namespace vsm
