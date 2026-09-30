#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "database_reader.h"
#include "keyboard_layout.h"

namespace viewer {

struct HeatmapData {
  // Counts follow keyboard_layout() order; unknown rows are excluded.
  std::vector<std::uint64_t> counts;
  std::uint64_t maximum = 0;
  std::size_t unmapped = 0;
  std::vector<KeyCountRow> top_keys;
};

HeatmapData make_heatmap_data(const DatabaseSnapshot& snapshot);
// -1 denotes zero; positive counts use levels 0..7.
int heat_level(std::uint64_t count, std::uint64_t maximum);
TerminalSize heatmap_minimum_size();
// A null snapshot means that no read has succeeded yet. Status is plain text.
std::string render_heatmap(const DatabaseSnapshot* snapshot, TerminalSize size,
                           std::string_view status);

}  // namespace viewer
