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
#include <stdexcept>
#include <string>

#include "database_reader.h"
#include "table_printer.h"

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

bool stop_requested(int signal_fd) {
  signalfd_siginfo signal{};
  while (true) {
    const ssize_t size = read(signal_fd, &signal, sizeof(signal));
    if (size == sizeof(signal)) {
      return true;
    }
    if (size < 0 && errno == EINTR) {
      continue;
    }
    if (size < 0 && errno == EAGAIN) {
      return false;
    }
    if (size < 0) {
      fail("Read exit signal");
    }
    throw std::runtime_error("Read exit signal: incomplete signal record");
  }
}

bool wait_for_next_read(int signal_fd, std::chrono::seconds interval) {
  const auto deadline = std::chrono::steady_clock::now() + interval;
  while (!stop_requested(signal_fd)) {
    const auto remaining = deadline - std::chrono::steady_clock::now();
    if (remaining <= std::chrono::steady_clock::duration::zero()) {
      return false;
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
  }
  return true;
}

void print_usage(const char* program, std::ostream& output) {
  output << "Usage: " << program
         << " [--db PATH] [--interval SECONDS] [--help]\n"
         << "Defaults: --db ./stats.db --interval 5\n"
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
    for (int index = 1; index < argc; ++index) {
      const std::string argument = argv[index];
      if (argument == "--help") {
        print_usage(argv[0], std::cout);
        return 0;
      }
      if (argument != "--db" && argument != "--interval") {
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

    sigset_t signal_mask;
    sigemptyset(&signal_mask);
    sigaddset(&signal_mask, SIGINT);
    sigaddset(&signal_mask, SIGTERM);
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
    while (!stop_requested(signal_fd.get())) {
      try {
        const auto snapshot = reader.read_snapshot();
        if (stop_requested(signal_fd.get())) {
          break;
        }
        viewer::print_snapshot(snapshot, std::cout);
      } catch (const viewer::DatabaseReadError& error) {
        if (!error.retryable()) {
          throw;
        }
        std::cerr << error.what() << "; retrying next interval\n";
      }
      if (wait_for_next_read(signal_fd.get(),
                             std::chrono::seconds(interval_seconds))) {
        break;
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
