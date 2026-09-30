#pragma once

#include <span>
#include <string_view>

namespace viewer {

struct TerminalSize {
  int columns;
  int rows;
};

struct KeyDefinition {
  std::string_view key_name;
  std::string_view label;
  int x;
  int y;
  int width;
  int height;
};

std::span<const KeyDefinition> keyboard_layout();
TerminalSize keyboard_size();

}  // namespace viewer
