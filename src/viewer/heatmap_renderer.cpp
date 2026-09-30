#include "heatmap_renderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <unordered_map>

namespace viewer {
namespace {

constexpr int header_rows = 2;
constexpr int footer_rows = 8;
constexpr std::array<int, 8> palette{223, 222, 221, 220, 214, 208, 202, 196};

struct Cell {
  char character = ' ';
  int style = -2;  // -2 = terminal default, -1 = zero, 0..7 = heat level
};

std::string color(int style) {
  if (style == -2) {
    return "\033[0m";
  }
  const int background = style == -1 ? 236 : palette.at(style);
  const int foreground = style == -1 ? 255 : 16;
  return "\033[38;5;" + std::to_string(foreground) + ";48;5;" +
         std::to_string(background) + "m";
}

// External error strings can contain paths or database text. Keep them from
// injecting control sequences or changing the width of this ASCII display.
std::string safe_text(std::string_view text) {
  std::string result;
  for (unsigned char character : text) {
    result += character >= 0x20 && character <= 0x7e
                  ? static_cast<char>(character)
                  : '?';
  }
  return result;
}

std::string timestamp(const DatabaseSnapshot* snapshot) {
  if (!snapshot) {
    return "Waiting for first successful read";
  }
  const auto time = std::chrono::system_clock::to_time_t(snapshot->read_at);
  std::tm utc{};
  if (!gmtime_r(&time, &utc)) {
    throw std::runtime_error("Format heatmap timestamp failed");
  }
  std::ostringstream result;
  result << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
  return result.str();
}

}  // namespace

HeatmapData make_heatmap_data(const DatabaseSnapshot& snapshot) {
  HeatmapData data;
  const auto layout = keyboard_layout();
  data.counts.resize(layout.size());
  std::unordered_map<std::string_view, std::size_t> indices;
  for (std::size_t index = 0; index < layout.size(); ++index) {
    indices.emplace(layout[index].key_name, index);
  }
  for (const auto& row : snapshot.rows) {
    const auto entry = indices.find(row.key_name);
    if (entry == indices.end()) {
      ++data.unmapped;
      continue;
    }
    data.counts[entry->second] = row.press_count;
    data.maximum = std::max(data.maximum, row.press_count);
    if (row.press_count > 0) {
      data.top_keys.push_back(row);
    }
  }
  std::sort(data.top_keys.begin(), data.top_keys.end(),
            [](const auto& left, const auto& right) {
              return left.press_count != right.press_count
                         ? left.press_count > right.press_count
                         : left.key_name < right.key_name;
            });
  if (data.top_keys.size() > 5) {
    data.top_keys.resize(5);
  }
  return data;
}

int heat_level(std::uint64_t count, std::uint64_t maximum) {
  if (count == 0 || maximum == 0) {
    return -1;
  }
  const double normalized = std::log1p(static_cast<double>(count)) /
                            std::log1p(static_cast<double>(maximum));
  return std::clamp(static_cast<int>(std::floor(8 * normalized)), 0, 7);
}

TerminalSize heatmap_minimum_size() {
  const auto keyboard = keyboard_size();
  // Two margins and one unused column prevent terminal auto-wrap.
  return {keyboard.columns + 3, header_rows + keyboard.rows + footer_rows};
}

std::string render_heatmap(const DatabaseSnapshot* snapshot, TerminalSize size,
                           std::string_view status) {
  const auto minimum = heatmap_minimum_size();
  const bool fits =
      size.columns >= minimum.columns && size.rows >= minimum.rows;
  // Bound allocations even when a terminal advertises an enormous window.
  const int width =
      std::max(0, std::min(size.columns - 1, minimum.columns - 1));
  const int height = std::max(0, std::min(size.rows, minimum.rows));
  std::vector<std::vector<Cell>> canvas(height, std::vector<Cell>(width));
  const auto text = [&](int x, int y, std::string_view value, int style = -2) {
    if (y < 0 || y >= height) {
      return;
    }
    const auto safe = safe_text(value);
    for (char character : safe) {
      if (x >= width) {
        break;
      }
      if (x >= 0) {
        canvas[y][x] = {character, style};
      }
      ++x;
    }
  };

  if (!fits) {
    text(0, 0,
         "Terminal too small: need " + std::to_string(minimum.columns) + "x" +
             std::to_string(minimum.rows) + ", current " +
             std::to_string(size.columns) + "x" + std::to_string(size.rows));
    text(0, 1, "Resize to show the full 104-key keyboard. Ctrl+C to exit.");
    text(0, 2, "Last read: " + timestamp(snapshot));
    text(0, 3, "Status: " + std::string(status));
  } else {
    const auto data =
        make_heatmap_data(snapshot ? *snapshot : DatabaseSnapshot{});
    text(1, 0, "KeyboardEyes | ANSI 104 | Cumulative key counts");
    text(1, 1, "Last read: " + timestamp(snapshot));
    const auto layout = keyboard_layout();
    for (std::size_t index = 0; index < layout.size(); ++index) {
      const auto& key = layout[index];
      const int style = heat_level(data.counts[index], data.maximum);
      const int x = key.x + 1;
      const int y = key.y + header_rows;
      for (int row = 0; row < key.height; ++row) {
        const bool edge = row == 0 || row == key.height - 1;
        text(x, y + row,
             std::string(edge ? "+" : "|") +
                 std::string(key.width - 2, edge ? '-' : ' ') +
                 (edge ? "+" : "|"),
             style);
      }
      text(x + (key.width - static_cast<int>(key.label.size())) / 2,
           y + key.height / 2, key.label, style);
    }
    const int footer = header_rows + keyboard_size().rows + 1;
    text(1, footer, "Heat: ");
    text(7, footer, " zero ", -1);
    for (int level = 0; level < 8; ++level) {
      text(14 + level * 4, footer, " " + std::to_string(level + 1) + " ",
           level);
    }
    text(47, footer, "low -> high | log scale relative to current maximum");
    text(1, footer + 1,
         "Maximum (mapped keys): " + std::to_string(data.maximum));
    text(1, footer + 2,
         "Top keys: " + std::string(data.top_keys.empty() ? "none" : ""));
    int x = 11;
    int y = footer + 2;
    for (std::size_t index = 0; index < data.top_keys.size(); ++index) {
      if (index == 3) {
        x = 11;
        ++y;
      }
      const auto& row = data.top_keys[index];
      const auto value = row.key_name + "=" + std::to_string(row.press_count);
      text(x, y, value);
      x += static_cast<int>(value.size()) + 3;
    }
    text(1, footer + 4,
         "Unmapped database rows: " + std::to_string(data.unmapped) +
             " (use --view table for all records)");
    text(1, footer + 5, "Status: " + std::string(status));
    text(1, footer + 6,
         "Ctrl+C to exit | Color shows cumulative usage, not currently held "
         "keys");
  }

  std::string frame = "\033[0m\033[H";
  for (int row = 0; row < height; ++row) {
    if (row > 0) {
      frame += "\r\n";
    }
    int style = -2;
    // Empty trailing cells are erased, avoiding wrap and stale wider frames.
    int end = width;
    while (end > 0 && canvas[row][end - 1].character == ' ' &&
           canvas[row][end - 1].style == -2) {
      --end;
    }
    for (int column = 0; column < end; ++column) {
      const auto& cell = canvas[row][column];
      if (cell.style != style) {
        frame += color(cell.style);
        style = cell.style;
      }
      frame += cell.character;
    }
    frame += "\033[0m\033[K";
  }
  frame += "\033[J";
  return frame;
}

}  // namespace viewer
