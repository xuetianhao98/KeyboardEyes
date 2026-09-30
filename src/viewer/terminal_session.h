#pragma once

#include <string_view>

#include "keyboard_layout.h"

namespace viewer {

enum class ViewMode { Auto, Keyboard, Table };
ViewMode parse_view_mode(std::string_view value);
bool use_keyboard_view(ViewMode mode);

class TerminalSession {
 public:
  explicit TerminalSession(bool enabled);
  ~TerminalSession();
  TerminalSession(const TerminalSession&) = delete;
  TerminalSession& operator=(const TerminalSession&) = delete;

  TerminalSize size() const;
  void present(std::string_view frame);

 private:
  bool enabled_;
};

}  // namespace viewer
