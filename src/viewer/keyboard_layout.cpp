#include "keyboard_layout.h"

#include <algorithm>

namespace viewer {
namespace {

// Character coordinates for US ANSI 104. Gaps remain uncolored; double-height
// numpad keys span the gap between rows. No libevdev dependency is needed here.
constexpr KeyDefinition keys[] = {
    {"KEY_ESC", "Esc", 0, 0, 5, 3},
    {"KEY_F1", "F1", 12, 0, 5, 3},
    {"KEY_F2", "F2", 18, 0, 5, 3},
    {"KEY_F3", "F3", 24, 0, 5, 3},
    {"KEY_F4", "F4", 30, 0, 5, 3},
    {"KEY_F5", "F5", 39, 0, 5, 3},
    {"KEY_F6", "F6", 45, 0, 5, 3},
    {"KEY_F7", "F7", 51, 0, 5, 3},
    {"KEY_F8", "F8", 57, 0, 5, 3},
    {"KEY_F9", "F9", 66, 0, 5, 3},
    {"KEY_F10", "F10", 72, 0, 5, 3},
    {"KEY_F11", "F11", 78, 0, 5, 3},
    {"KEY_F12", "F12", 84, 0, 5, 3},
    {"KEY_SYSRQ", "Prt", 93, 0, 5, 3},
    {"KEY_SCROLLLOCK", "Scr", 99, 0, 5, 3},
    {"KEY_PAUSE", "Pau", 105, 0, 5, 3},

    {"KEY_GRAVE", "`", 0, 4, 5, 3},
    {"KEY_1", "1", 6, 4, 5, 3},
    {"KEY_2", "2", 12, 4, 5, 3},
    {"KEY_3", "3", 18, 4, 5, 3},
    {"KEY_4", "4", 24, 4, 5, 3},
    {"KEY_5", "5", 30, 4, 5, 3},
    {"KEY_6", "6", 36, 4, 5, 3},
    {"KEY_7", "7", 42, 4, 5, 3},
    {"KEY_8", "8", 48, 4, 5, 3},
    {"KEY_9", "9", 54, 4, 5, 3},
    {"KEY_0", "0", 60, 4, 5, 3},
    {"KEY_MINUS", "-", 66, 4, 5, 3},
    {"KEY_EQUAL", "=", 72, 4, 5, 3},
    {"KEY_BACKSPACE", "Backspace", 78, 4, 11, 3},
    {"KEY_INSERT", "Ins", 93, 4, 5, 3},
    {"KEY_HOME", "Hom", 99, 4, 5, 3},
    {"KEY_PAGEUP", "PgU", 105, 4, 5, 3},
    {"KEY_NUMLOCK", "Num", 114, 4, 5, 3},
    {"KEY_KPSLASH", "/", 120, 4, 5, 3},
    {"KEY_KPASTERISK", "*", 126, 4, 5, 3},
    {"KEY_KPMINUS", "-", 132, 4, 5, 3},

    {"KEY_TAB", "Tab", 0, 8, 8, 3},
    {"KEY_Q", "Q", 9, 8, 5, 3},
    {"KEY_W", "W", 15, 8, 5, 3},
    {"KEY_E", "E", 21, 8, 5, 3},
    {"KEY_R", "R", 27, 8, 5, 3},
    {"KEY_T", "T", 33, 8, 5, 3},
    {"KEY_Y", "Y", 39, 8, 5, 3},
    {"KEY_U", "U", 45, 8, 5, 3},
    {"KEY_I", "I", 51, 8, 5, 3},
    {"KEY_O", "O", 57, 8, 5, 3},
    {"KEY_P", "P", 63, 8, 5, 3},
    {"KEY_LEFTBRACE", "[", 69, 8, 5, 3},
    {"KEY_RIGHTBRACE", "]", 75, 8, 5, 3},
    {"KEY_BACKSLASH", "\\", 81, 8, 8, 3},
    {"KEY_DELETE", "Del", 93, 8, 5, 3},
    {"KEY_END", "End", 99, 8, 5, 3},
    {"KEY_PAGEDOWN", "PgD", 105, 8, 5, 3},
    {"KEY_KP7", "7", 114, 8, 5, 3},
    {"KEY_KP8", "8", 120, 8, 5, 3},
    {"KEY_KP9", "9", 126, 8, 5, 3},
    {"KEY_KPPLUS", "+", 132, 8, 5, 7},

    {"KEY_CAPSLOCK", "Caps", 0, 12, 10, 3},
    {"KEY_A", "A", 11, 12, 5, 3},
    {"KEY_S", "S", 17, 12, 5, 3},
    {"KEY_D", "D", 23, 12, 5, 3},
    {"KEY_F", "F", 29, 12, 5, 3},
    {"KEY_G", "G", 35, 12, 5, 3},
    {"KEY_H", "H", 41, 12, 5, 3},
    {"KEY_J", "J", 47, 12, 5, 3},
    {"KEY_K", "K", 53, 12, 5, 3},
    {"KEY_L", "L", 59, 12, 5, 3},
    {"KEY_SEMICOLON", ";", 65, 12, 5, 3},
    {"KEY_APOSTROPHE", "'", 71, 12, 5, 3},
    {"KEY_ENTER", "Enter", 77, 12, 12, 3},
    {"KEY_KP4", "4", 114, 12, 5, 3},
    {"KEY_KP5", "5", 120, 12, 5, 3},
    {"KEY_KP6", "6", 126, 12, 5, 3},

    {"KEY_LEFTSHIFT", "Shift", 0, 16, 13, 3},
    {"KEY_Z", "Z", 14, 16, 5, 3},
    {"KEY_X", "X", 20, 16, 5, 3},
    {"KEY_C", "C", 26, 16, 5, 3},
    {"KEY_V", "V", 32, 16, 5, 3},
    {"KEY_B", "B", 38, 16, 5, 3},
    {"KEY_N", "N", 44, 16, 5, 3},
    {"KEY_M", "M", 50, 16, 5, 3},
    {"KEY_COMMA", ",", 56, 16, 5, 3},
    {"KEY_DOT", ".", 62, 16, 5, 3},
    {"KEY_SLASH", "/", 68, 16, 5, 3},
    {"KEY_RIGHTSHIFT", "Shift", 74, 16, 15, 3},
    {"KEY_UP", "^", 99, 16, 5, 3},
    {"KEY_KP1", "1", 114, 16, 5, 3},
    {"KEY_KP2", "2", 120, 16, 5, 3},
    {"KEY_KP3", "3", 126, 16, 5, 3},
    {"KEY_KPENTER", "Ent", 132, 16, 5, 7},

    {"KEY_LEFTCTRL", "Ctrl", 0, 20, 7, 3},
    {"KEY_LEFTMETA", "Win", 8, 20, 6, 3},
    {"KEY_LEFTALT", "Alt", 15, 20, 7, 3},
    {"KEY_SPACE", "Space", 23, 20, 36, 3},
    {"KEY_RIGHTALT", "Alt", 60, 20, 7, 3},
    {"KEY_RIGHTMETA", "Win", 68, 20, 6, 3},
    {"KEY_COMPOSE", "Menu", 75, 20, 7, 3},
    {"KEY_RIGHTCTRL", "Ctrl", 83, 20, 6, 3},
    {"KEY_LEFT", "<", 93, 20, 5, 3},
    {"KEY_DOWN", "v", 99, 20, 5, 3},
    {"KEY_RIGHT", ">", 105, 20, 5, 3},
    {"KEY_KP0", "0", 114, 20, 11, 3},
    {"KEY_KPDOT", ".", 126, 20, 5, 3},
};

static_assert(std::size(keys) == 104);

}  // namespace

std::span<const KeyDefinition> keyboard_layout() { return keys; }

TerminalSize keyboard_size() {
  TerminalSize size{};
  for (const auto& key : keys) {
    size.columns = std::max(size.columns, key.x + key.width);
    size.rows = std::max(size.rows, key.y + key.height);
  }
  return size;
}

}  // namespace viewer
