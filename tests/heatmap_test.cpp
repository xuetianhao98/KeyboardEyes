#include <algorithm>
#include <iostream>
#include <limits>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "heatmap_renderer.h"

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

std::vector<std::string> plain_lines(const std::string& frame) {
  const auto plain =
      std::regex_replace(frame, std::regex("\033\\[[0-9;]*[A-Za-z]"), "");
  std::istringstream input(plain);
  std::vector<std::string> lines;
  std::string line;
  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    lines.push_back(line);
  }
  return lines;
}

std::size_t key_index(std::string_view name) {
  const auto layout = viewer::keyboard_layout();
  const auto found =
      std::find_if(layout.begin(), layout.end(),
                   [&](const auto& key) { return key.key_name == name; });
  require(found != layout.end(), "Missing key: " + std::string(name));
  return static_cast<std::size_t>(found - layout.begin());
}

void check_layout() {
  const auto layout = viewer::keyboard_layout();
  require(layout.size() == 104, "Expected ANSI 104 keys");
  std::set<std::string_view> names;
  std::set<std::pair<int, int>> cells;
  for (const auto& key : layout) {
    require(names.insert(key.key_name).second, "Duplicate key name");
    require(key.width >= 3 && key.height >= 3 && key.x >= 0 && key.y >= 0,
            "Invalid key geometry");
    require(key.label.size() <= static_cast<std::size_t>(key.width - 2),
            "Key label does not fit");
    for (char character : key.label) {
      require(character >= 0x20 && character <= 0x7e, "Label must be ASCII");
    }
    for (int y = key.y; y < key.y + key.height; ++y) {
      for (int x = key.x; x < key.x + key.width; ++x) {
        require(cells.emplace(x, y).second, "Overlapping keycaps");
      }
    }
  }
  require(viewer::keyboard_size().columns == 137 &&
              viewer::keyboard_size().rows == 23,
          "Unexpected keyboard bounds");
  require(layout[key_index("KEY_KPENTER")].height == 7 &&
              layout[key_index("KEY_KPPLUS")].height == 7,
          "Numpad Enter and plus must span two rows");
  key_index("KEY_SYSRQ");
  key_index("KEY_COMPOSE");
  require(layout[key_index("KEY_A")].x > layout[key_index("KEY_Q")].x &&
              layout[key_index("KEY_Z")].x > layout[key_index("KEY_A")].x,
          "Letter rows must be staggered");
}

void check_data() {
  viewer::DatabaseSnapshot snapshot{};
  snapshot.rows = {{1, "KEY_LEFTSHIFT", 1},
                   {2, "KEY_RIGHTSHIFT", 2},
                   {3, "KEY_LEFTCTRL", 3},
                   {4, "KEY_RIGHTCTRL", 4},
                   {5, "KEY_LEFTALT", 5},
                   {6, "KEY_RIGHTALT", 6},
                   {7, "KEY_LEFTMETA", 7},
                   {8, "KEY_RIGHTMETA", 8},
                   {9, "KEY_1", 9},
                   {10, "KEY_KP1", 10},
                   {11, "KEY_ENTER", 11},
                   {12, "KEY_KPENTER", 12},
                   {13, "KEY_VOLUMEUP", 999999}};
  const auto data = viewer::make_heatmap_data(snapshot);
  for (const auto& row : snapshot.rows) {
    if (row.key_name == "KEY_VOLUMEUP") continue;
    require(data.counts[key_index(row.key_name)] == row.press_count,
            "Key mapping mixed distinct positions");
  }
  require(data.maximum == 12 && data.unmapped == 1,
          "Unknown key changed normalization");
  require(data.counts[key_index("KEY_SPACE")] == 0,
          "Missing keys must be zero");
  require(data.top_keys.size() == 5 &&
              data.top_keys.front().key_name == "KEY_KPENTER",
          "Top five must exclude unknown keys");

  snapshot.rows = {{1, "KEY_B", 10}, {2, "KEY_A", 10}, {3, "KEY_C", 0}};
  const auto tied = viewer::make_heatmap_data(snapshot);
  require(tied.top_keys.size() == 2 && tied.top_keys[0].key_name == "KEY_A" &&
              tied.top_keys[1].key_name == "KEY_B",
          "Top key ties must sort by name");
  require(viewer::make_heatmap_data({}).top_keys.empty(),
          "Empty data must have no top keys");
}

void check_colors() {
  const auto maximum =
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  require(
      viewer::heat_level(0, 0) == -1 && viewer::heat_level(0, maximum) == -1,
      "Zero must be neutral");
  require(viewer::heat_level(1, 1) == 7 &&
              viewer::heat_level(maximum, maximum) == 7,
          "A maximum must use the hottest color");
  int previous = -1;
  for (std::uint64_t count : {1ULL, 2ULL, 10ULL, 1000ULL, 1000000ULL,
                              1000000000000ULL, 9223372036854775807ULL}) {
    const int level = viewer::heat_level(count, maximum);
    require(level >= previous && level >= 0 && level <= 7,
            "Heat must be monotonic and bounded");
    previous = level;
  }
  require(viewer::heat_level(10, 1000) > 0,
          "Logarithmic scale must expose low-frequency keys");
}

void check_frame() {
  viewer::DatabaseSnapshot snapshot{};
  const auto maximum =
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  snapshot.rows = {
      {1, "KEY_RIGHTSHIFT", maximum}, {2, "KEY_LEFTSHIFT", maximum},
      {3, "KEY_SCROLLLOCK", maximum}, {4, "KEY_BACKSPACE", maximum},
      {5, "KEY_RIGHTCTRL", maximum},  {6, "UNKNOWN_999", maximum}};
  const auto minimum = viewer::heatmap_minimum_size();
  require(minimum.columns == 140 && minimum.rows == 33,
          "Unexpected frame bounds");
  const auto frame =
      viewer::render_heatmap(&snapshot, minimum, "Read successful");
  const auto lines = plain_lines(frame);
  require(lines.size() <= static_cast<std::size_t>(minimum.rows),
          "Frame is too tall");
  for (const auto& line : lines) {
    require(line.size() < static_cast<std::size_t>(minimum.columns),
            "Frame would auto-wrap");
  }
  for (const auto& key : viewer::keyboard_layout()) {
    const auto& line = lines.at(key.y + 2 + key.height / 2);
    const auto x =
        key.x + 1 + (key.width - static_cast<int>(key.label.size())) / 2;
    require(line.substr(x, key.label.size()) == key.label,
            "Rendered key label is misplaced");
  }
  for (const auto& row : snapshot.rows) {
    if (row.key_name == "UNKNOWN_999") continue;
    require(frame.find(row.key_name + "=" + std::to_string(maximum)) !=
                std::string::npos,
            "Top count was truncated or rounded");
  }
  require(frame.find("Unmapped database rows: 1") != std::string::npos,
          "Missing unmapped count");
  require(frame.find("48;5;236m") != std::string::npos &&
              frame.find("48;5;196m") != std::string::npos,
          "Missing neutral or hottest colors");
  require(frame.find("1970-01-01T00:00:00Z") != std::string::npos,
          "Timestamp must use UTC");

  for (viewer::TerminalSize size :
       {viewer::TerminalSize{80, 24}, {1, 1}, {0, 0}, {140, 1}}) {
    const auto small = viewer::render_heatmap(nullptr, size, "busy\033[2J\n");
    const auto small_lines = plain_lines(small);
    require(small_lines.size() <= static_cast<std::size_t>(size.rows),
            "Small frame too tall");
    for (const auto& line : small_lines) {
      require(line.size() <=
                  static_cast<std::size_t>(std::max(0, size.columns - 1)),
              "Small frame too wide");
    }
    require(small.find("busy\033") == std::string::npos,
            "Status injected terminal control codes");
  }
  const auto waiting =
      viewer::render_heatmap(nullptr, minimum, "retrying next interval");
  require(
      waiting.find("Waiting for first successful read") != std::string::npos,
      "First-read failure must not pretend to have a snapshot");
}

}  // namespace

int main() {
  try {
    check_layout();
    check_data();
    check_colors();
    check_frame();
    std::cout << "Layout, mapping, colors and frame checks passed.\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
