#include <fcntl.h>
#include <libevdev/libevdev.h>
#include <linux/input.h>
#include <unistd.h>

#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <unordered_map>

int main(int argc, char* argv[]) {
  if (argc != 2) {
    std::fprintf(stderr, "Usage: %s /dev/input/eventX\n", argv[0]);
    return 1;
  }
  const int fd = open(argv[1], O_RDONLY | O_CLOEXEC);
  if (fd == -1) {
    std::perror("Open device error!");
    return 1;
  }
  libevdev* dev = nullptr;
  int rc = libevdev_new_from_fd(fd, &dev);
  if (rc < 0) {
    std::fprintf(stderr, "Init error: %s\n", std::strerror(-rc));
    close(fd);
    return 1;
  }

  std::printf("Start listening: %s\n", libevdev_get_name(dev));
  std::unordered_map<unsigned int, std::uint64_t> press_counts;
  bool syncing = false;
  int result = 0;
  input_event event{};
  while (true) {
    const unsigned int flags =
        syncing ? LIBEVDEV_READ_FLAG_SYNC
                : LIBEVDEV_READ_FLAG_NORMAL | LIBEVDEV_READ_FLAG_BLOCKING;
    rc = libevdev_next_event(dev, flags, &event);

    if (rc == -EINTR) {
      continue;
    }
    if (rc == -EAGAIN) {
      // Sync mode ends when the library has consumed all state differences.
      syncing = false;
      continue;
    }
    if (rc < 0) {
      std::fprintf(stderr, "Read error: %s\n", std::strerror(-rc));
      result = 1;
      break;
    }
    if (rc == LIBEVDEV_READ_STATUS_SYNC) {
      if (!syncing) {
        std::fprintf(stderr, "Events dropped; syncing device state...\n");
      }
      syncing = true;
      // Update cached state without reporting recovery events as keystrokes.
      continue;
    }
    if (event.type == EV_KEY && event.value == 1) {
      const auto count = ++press_counts[event.code];
      const char* name = libevdev_event_code_get_name(event.type, event.code);
      std::printf("%s %" PRIu64 "\n", name ? name : "UNKNOWN", count);
      std::fflush(stdout);
    }
  }
  libevdev_free(dev);
  close(fd);
  return result;
}
