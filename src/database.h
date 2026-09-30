#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

struct sqlite3;
struct sqlite3_stmt;

class Database {
 public:
  explicit Database(const std::string& path);
  ~Database();

  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;
  Database(Database&&) = delete;
  Database& operator=(Database&&) = delete;

  std::unordered_map<std::string, std::uint64_t> load_counts();

  // Saves an absolute total in the range 0..INT64_MAX, not an increment.
  void save_count(const std::string& key_name, std::uint64_t total_count);

 private:
  sqlite3* db_ = nullptr;
  sqlite3_stmt* save_statement_ = nullptr;
};
