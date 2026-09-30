#pragma once

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

struct sqlite3;

namespace viewer {

struct KeyCountRow {
  std::int64_t id;
  std::string key_name;
  std::uint64_t press_count;
};

struct DatabaseSnapshot {
  std::chrono::system_clock::time_point read_at;
  std::vector<KeyCountRow> rows;
};

class DatabaseReadError : public std::runtime_error {
 public:
  DatabaseReadError(int code, const std::string& message);
  bool retryable() const;

 private:
  int code_;
};

class DatabaseReader {
 public:
  explicit DatabaseReader(const std::string& path);
  ~DatabaseReader();

  DatabaseReader(const DatabaseReader&) = delete;
  DatabaseReader& operator=(const DatabaseReader&) = delete;
  DatabaseReader(DatabaseReader&&) = delete;
  DatabaseReader& operator=(DatabaseReader&&) = delete;

  // Returns owned data after releasing the query and its read transaction.
  DatabaseSnapshot read_snapshot();

 private:
  sqlite3* db_ = nullptr;
  std::string path_;
};

}  // namespace viewer
