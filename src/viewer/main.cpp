#include <poll.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstring>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>

#include "database_reader.h"
#include "heatmap_renderer.h"
#include "table_printer.h"
#include "terminal_session.h"

namespace {

class ScopedFd {
 public:
  explicit ScopedFd(int fd) : fd_(fd) {}
  ~ScopedFd() {
    if (fd_ >= 0) {
      close(fd_);
    }
  }
  ScopedFd(const ScopedFd&) = delete;
  ScopedFd& operator=(const ScopedFd&) = delete;
  int get() const { return fd_; }

 private:
  int fd_;
};

[[noreturn]] void fail(const char* operation) {
  throw std::runtime_error(std::string(operation) + ": " +
                           std::strerror(errno));
}

struct Signals {
  bool stop = false;
  bool resize = false;
};

Signals pending_signals(int signal_fd) {
  Signals result;
  signalfd_siginfo signal{};
  while (true) {
    const ssize_t size = read(signal_fd, &signal, sizeof(signal));
    if (size == sizeof(signal)) {
      result.stop |= signal.ssi_signo == SIGINT || signal.ssi_signo == SIGTERM;
      result.resize |= signal.ssi_signo == SIGWINCH;
      continue;
    }
    if (size < 0 && errno == EINTR) {
      continue;
    }
    if (size < 0 && errno == EAGAIN) {
      return result;
    }
    if (size < 0) {
      fail("Read exit signal");
    }
    throw std::runtime_error("Read exit signal: incomplete signal record");
  }
}

void wait_for_event(int signal_fd,
                    std::chrono::steady_clock::time_point deadline) {
  while (true) {
    const auto remaining = deadline - std::chrono::steady_clock::now();
    if (remaining <= std::chrono::steady_clock::duration::zero()) {
      return;
    }
    const int timeout = static_cast<int>(
        std::chrono::ceil<std::chrono::milliseconds>(remaining).count());
    pollfd descriptor{signal_fd, POLLIN, 0};
    if (poll(&descriptor, 1, timeout) < 0) {
      if (errno == EINTR) {
        continue;
      }
      fail("Wait for next database read");
    }
    if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) {
      throw std::runtime_error("Exit signal descriptor became unavailable");
    }
    return;
  }
}

void print_usage(const char* program, std::ostream& output) {
  output
      << "Usage: " << program
      << " [--db PATH] [--interval SECONDS] [--view auto|keyboard|table] "
         "[--help]\n"
      << "Defaults: --db ./stats.db --interval 5 --view auto\n"
      << "Auto uses a keyboard heatmap on a 256-color TTY, otherwise a table.\n"
      << "SECONDS must be an integer in 1.."
      << std::numeric_limits<int>::max() / 1000 << ".\n";
}

}  // namespace

int main(int argc, char* argv[]) {
  try {
    std::string database_path = "./stats.db";
    int interval_seconds = 5;
    bool path_set = false;
    bool interval_set = false;
    bool view_set = false;
    viewer::ViewMode view_mode = viewer::ViewMode::Auto;
    for (int index = 1; index < argc; ++index) {
      const std::string argument = argv[index];
      if (argument == "--help") {
        print_usage(argv[0], std::cout);
        return 0;
      }
      if (argument != "--db" && argument != "--interval" &&
          argument != "--view") {
        throw std::invalid_argument("Unknown argument: " + argument);
      }
      if (index + 1 == argc || argv[index + 1][0] == '\0' ||
          argv[index + 1][0] == '-') {
        throw std::invalid_argument("Missing or invalid value for " + argument);
      }
      const std::string value = argv[++index];
      if (argument == "--db") {
        if (path_set) {
          throw std::invalid_argument("Repeated --db option");
        }
        database_path = value;
        path_set = true;
      } else if (argument == "--view") {
        if (view_set) {
          throw std::invalid_argument("Repeated --view option");
        }
        view_mode = viewer::parse_view_mode(value);
        view_set = true;
      } else {
        if (interval_set) {
          throw std::invalid_argument("Repeated --interval option");
        }
        const auto [end, error] = std::from_chars(
            value.data(), value.data() + value.size(), interval_seconds);
        if (error != std::errc{} || end != value.data() + value.size() ||
            interval_seconds <= 0 ||
            interval_seconds > std::numeric_limits<int>::max() / 1000) {
          throw std::invalid_argument("Invalid --interval: " + value);
        }
        interval_set = true;
      }
    }

    const bool keyboard_view = viewer::use_keyboard_view(view_mode);
    sigset_t signal_mask;
    sigemptyset(&signal_mask);
    sigaddset(&signal_mask, SIGINT);
    sigaddset(&signal_mask, SIGTERM);
    sigaddset(&signal_mask, SIGWINCH);
    if (sigprocmask(SIG_BLOCK, &signal_mask, nullptr) < 0) {
      fail("Block exit signals");
    }
    const ScopedFd signal_fd(
        signalfd(-1, &signal_mask, SFD_NONBLOCK | SFD_CLOEXEC));
    if (signal_fd.get() < 0) {
      fail("Create signal descriptor");
    }
    // A closed output pipe should take the normal error-reporting path.
    struct sigaction ignore_pipe{};
    ignore_pipe.sa_handler = SIG_IGN;
    sigemptyset(&ignore_pipe.sa_mask);
    if (sigaction(SIGPIPE, &ignore_pipe, nullptr) < 0) {
      fail("Ignore broken pipe signal");
    }

    viewer::DatabaseReader reader(database_path);
    std::cerr << "Started viewer; database: " << database_path
              << "; interval: " << interval_seconds << " seconds\n";
    {
      // Scope the alternate screen so both normal and exceptional exits restore
      // it before the final stderr message is printed.
      viewer::TerminalSession terminal(keyboard_view);
      std::optional<viewer::DatabaseSnapshot> snapshot;
      std::string status = "Waiting for first successful read";
      auto deadline = std::chrono::steady_clock::now();
      while (true) {
        const auto signals = pending_signals(signal_fd.get());
        if (signals.stop) break;
        bool redraw = keyboard_view && signals.resize;
        const bool read_due = std::chrono::steady_clock::now() >= deadline;
        bool read_succeeded = false;
        if (read_due) {
          try {
            snapshot = reader.read_snapshot();
            status = "Read successful";
            read_succeeded = true;
          } catch (const viewer::DatabaseReadError& error) {
            if (!error.retryable()) throw;
            status = "Read busy; retrying next interval. " +
                     std::string(error.what());
            if (!keyboard_view) {
              std::cerr << error.what() << "; retrying next interval\n";
            }
          }
          redraw = keyboard_view;
        }
        const auto after_read = pending_signals(signal_fd.get());
        if (after_read.stop) break;
        redraw |= keyboard_view && after_read.resize;
        if (redraw) {
          terminal.present(viewer::render_heatmap(
              snapshot ? &*snapshot : nullptr, terminal.size(), status));
        } else if (read_succeeded) {
          viewer::print_snapshot(*snapshot, std::cout);
        }
        if (read_due) {
          deadline = std::chrono::steady_clock::now() +
                     std::chrono::seconds(interval_seconds);
        }
        wait_for_event(signal_fd.get(), deadline);
      }
    }
    std::cerr << "Stopping viewer.\n";
    return 0;
  } catch (const std::invalid_argument& error) {
    std::cerr << error.what() << '\n';
    print_usage(argv[0], std::cerr);
    return 1;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
