#include <fcntl.h>
#include <libevdev/libevdev.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include "database.h"

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

[[noreturn]] void fail(const char* operation, int error) {
  throw std::runtime_error(std::string(operation) + ": " +
                           std::strerror(error));
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
      fail("Read exit signal", errno);
    }
    throw std::runtime_error("Read exit signal: incomplete signal record");
  }
}

bool reconnectable(int error) {
  return error == ENOENT || error == ENODEV || error == ENXIO || error == EIO;
}

// Return true if shutdown was requested, false when it is time to reconnect.
bool wait_for_retry(int signal_fd) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
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
      fail("Wait to reconnect", errno);
    }
    if (descriptor.revents & POLLIN) {
      continue;
    }
    if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) {
      throw std::runtime_error("Exit signal descriptor became unavailable");
    }
  }
  return true;
}

// Zero means shutdown; a positive errno means the connection was lost.
int listen_to_device(
    libevdev* device, int device_fd, int signal_fd, Database& database,
    std::unordered_map<std::string, std::uint64_t>& press_counts) {
  bool syncing = false;
  input_event event{};
  while (!stop_requested(signal_fd)) {
    const unsigned int flags =
        syncing ? LIBEVDEV_READ_FLAG_SYNC : LIBEVDEV_READ_FLAG_NORMAL;
    const int rc = libevdev_next_event(device, flags, &event);
    if (rc == -EINTR) {
      continue;
    }
    if (rc == -EAGAIN) {
      if (syncing) {
        // Resume normal reads before waiting: libevdev may have cached events.
        syncing = false;
        continue;
      }
      pollfd descriptors[] = {{signal_fd, POLLIN, 0}, {device_fd, POLLIN, 0}};
      if (poll(descriptors, 2, -1) < 0) {
        if (errno == EINTR) {
          continue;
        }
        fail("Wait for input", errno);
      }
      // Give an exit request priority over device errors arriving together.
      if (descriptors[0].revents & POLLIN) {
        continue;
      }
      if (descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
        throw std::runtime_error("Exit signal descriptor became unavailable");
      }
      if (descriptors[1].revents & POLLNVAL) {
        fail("Poll input device", EBADF);
      }
      if (descriptors[1].revents & POLLHUP) {
        return ENODEV;
      }
      if (descriptors[1].revents & POLLERR) {
        return EIO;
      }
      continue;
    }
    if (rc == -ENODEV || rc == -ENXIO || rc == -EIO) {
      return -rc;
    }
    if (rc < 0) {
      fail("Read input", -rc);
    }
    if (rc == LIBEVDEV_READ_STATUS_SYNC) {
      if (!syncing) {
        std::fprintf(stderr, "Events dropped; syncing device state...\n");
      }
      syncing = true;
      // Recovery events describe state, not new presses.
      continue;
    }
    if (event.type == EV_KEY && event.value == 1) {
      const char* name = libevdev_event_code_get_name(event.type, event.code);
      const std::string key_name =
          name ? name : "UNKNOWN_" + std::to_string(event.code);
      auto& count = press_counts[key_name];
      if (count >= static_cast<std::uint64_t>(
                       std::numeric_limits<std::int64_t>::max())) {
        throw std::runtime_error("Count limit reached for " + key_name);
      }
      const auto next_count = count + 1;
      database.save_count(key_name, next_count);
      count = next_count;
      std::printf("%s %" PRIu64 "\n", key_name.c_str(), count);
      std::fflush(stdout);
    }
  }
  return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
  try {
    std::string device_path;
    std::string database_path = "./stats.db";
    bool database_path_set = false;
    const auto usage = [&] {
      std::fprintf(stderr, "Usage: %s /dev/input/eventX [--db PATH]\n",
                   argv[0]);
      return 1;
    };
    for (int i = 1; i < argc; ++i) {
      const std::string argument = argv[i];
      if (argument == "--db") {
        if (database_path_set || i + 1 == argc || argv[i + 1][0] == '\0' ||
            argv[i + 1][0] == '-') {
          return usage();
        }
        database_path = argv[++i];
        database_path_set = true;
      } else if (argument.empty() || argument.front() == '-' ||
                 !device_path.empty()) {
        return usage();
      } else {
        device_path = argument;
      }
    }
    if (device_path.empty()) {
      return usage();
    }

    // Receive termination signals through the event loop, including while idle.
    sigset_t signal_mask;
    sigemptyset(&signal_mask);
    sigaddset(&signal_mask, SIGINT);
    sigaddset(&signal_mask, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &signal_mask, nullptr) < 0) {
      fail("Block exit signals", errno);
    }
    const ScopedFd signal_fd(
        signalfd(-1, &signal_mask, SFD_NONBLOCK | SFD_CLOEXEC));
    if (signal_fd.get() < 0) {
      fail("Create signal descriptor", errno);
    }

    Database database(database_path);
    auto press_counts = database.load_counts();
    bool connected_before = false;
    int last_error = 0;
    while (!stop_requested(signal_fd.get())) {
      int error = 0;
      const char* operation = "Open input device";
      {
        // Each attempt resolves the original path again and owns fresh state.
        const ScopedFd device_fd(
            open(device_path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK));
        if (device_fd.get() < 0) {
          error = errno;
        } else {
          operation = "Initialize input device";
          libevdev* raw_device = nullptr;
          const int rc = libevdev_new_from_fd(device_fd.get(), &raw_device);
          const std::unique_ptr<libevdev, decltype(&libevdev_free)> device(
              raw_device, libevdev_free);
          if (rc < 0) {
            error = -rc;
          } else {
            const char* name = libevdev_get_name(device.get());
            std::printf(
                "%s: %s\n",
                connected_before ? "Device reconnected" : "Start listening",
                name ? name : "UNKNOWN");
            std::fflush(stdout);
            connected_before = true;
            last_error = 0;
            error = listen_to_device(device.get(), device_fd.get(),
                                     signal_fd.get(), database, press_counts);
          }
        }
      }  // Release libevdev before closing its fd, before any retry wait.
      if (error == 0 || stop_requested(signal_fd.get())) {
        return 0;
      }
      if (error == EINTR) {
        continue;
      }
      if (!reconnectable(error)) {
        fail(operation, error);
      }
      if (error != last_error) {
        std::fprintf(
            stderr,
            "Device '%s' unavailable: %s; retrying every 2 seconds...\n",
            device_path.c_str(), std::strerror(error));
        last_error = error;
      }
      if (wait_for_retry(signal_fd.get())) {
        return 0;
      }
    }
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
