#include "core/Selection.hpp"

namespace vsm {

std::size_t move_selection(std::size_t current, std::size_t item_count, int delta) {
  if (item_count == 0) {
    return 0;
  }

  // Controller input should wrap like a console menu instead of clamping at the ends. The modulo
  // expression is written in signed space first so negative movement from index zero lands on the
  // final item rather than underflowing a size_t.
  const int count = static_cast<int>(item_count);
  const int wrapped = (static_cast<int>(current % item_count) + delta + count) % count;
  return static_cast<std::size_t>(wrapped);
}

} // namespace vsm
