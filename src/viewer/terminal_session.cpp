#include "terminal_session.h"

#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

namespace viewer {
namespace {

constexpr std::string_view enter = "\033[?1049h\033[?25l\033[0m\033[2J\033[H";
constexpr std::string_view leave = "\033[0m\033[?25h\033[?1049l";

// Used for both normal output and best-effort cleanup, including after failure.
bool write_all(std::string_view text) {
  while (!text.empty()) {
    const auto written = write(STDOUT_FILENO, text.data(), text.size());
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written <= 0) {
      return false;
    }
    text.remove_prefix(static_cast<std::size_t>(written));
  }
  return true;
}

std::string_view environment(const char* name) {
  const char* value = std::getenv(name);
  return value ? value : "";
}

}  // namespace

ViewMode parse_view_mode(std::string_view value) {
  if (value == "auto") return ViewMode::Auto;
  if (value == "keyboard") return ViewMode::Keyboard;
  if (value == "table") return ViewMode::Table;
  throw std::invalid_argument("Invalid --view: " + std::string(value));
}

bool use_keyboard_view(ViewMode mode) {
  if (mode == ViewMode::Table) {
    return false;
  }
  const auto term = environment("TERM");
  const bool usable = isatty(STDOUT_FILENO) && !term.empty() && term != "dumb";
  if (mode == ViewMode::Keyboard) {
    if (!usable) {
      throw std::runtime_error(
          "--view keyboard requires a TTY with TERM set and not 'dumb'");
    }
    return true;
  }
  const auto color_term = environment("COLORTERM");
  return usable && (term.find("256color") != std::string_view::npos ||
                    color_term == "truecolor" || color_term == "24bit");
}

TerminalSession::TerminalSession(bool enabled) : enabled_(enabled) {
  if (enabled_ && !write_all(enter)) {
    const int error = errno;
    write_all(leave);
    throw std::runtime_error("Enter terminal screen: " +
                             std::string(std::strerror(error)));
  }
}

TerminalSession::~TerminalSession() {
  if (enabled_) {
    write_all(leave);
  }
}

TerminalSize TerminalSession::size() const {
  winsize size{};
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) < 0) {
    throw std::runtime_error("Read terminal size: " +
                             std::string(std::strerror(errno)));
  }
  return {size.ws_col, size.ws_row};
}

void TerminalSession::present(std::string_view frame) {
  if (!write_all(frame)) {
    throw std::runtime_error("Write heatmap to stdout failed: " +
                             std::string(std::strerror(errno)));
  }
}

}  // namespace viewer
