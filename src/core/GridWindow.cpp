#include "core/GridWindow.hpp"

#include <algorithm>

namespace vsm {

std::size_t grid_window_top_row(std::size_t current_top_row, std::size_t selected_index,
                                std::size_t item_count, std::size_t columns,
                                std::size_t visible_rows) {
  if (item_count == 0 || columns == 0 || visible_rows == 0) {
    return 0;
  }

  const std::size_t selected_row = selected_index / columns;
  const std::size_t total_rows = (item_count + columns - 1) / columns;
  const std::size_t max_top_row = total_rows <= visible_rows ? 0 : total_rows - visible_rows;
  const std::size_t top_row = std::min(current_top_row, max_top_row);
  const std::size_t bottom_row = top_row + visible_rows - 1;

  if (selected_row < top_row) {
    return selected_row;
  }
  if (selected_row > bottom_row) {
    return std::min(selected_row - visible_rows + 1, max_top_row);
  }
  return top_row;
}

} // namespace vsm
