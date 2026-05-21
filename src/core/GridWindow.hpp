#pragma once

#include <cstddef>

namespace vsm {

std::size_t grid_window_top_row(std::size_t current_top_row, std::size_t selected_index,
                                std::size_t item_count, std::size_t columns,
                                std::size_t visible_rows);

} // namespace vsm
