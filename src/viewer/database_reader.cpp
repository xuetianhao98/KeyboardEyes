#include "database_reader.h"

#include <sqlite3.h>
#include <sys/stat.h>

#include <cerrno>
#include <cstring>
#include <memory>

namespace viewer {

DatabaseReadError::DatabaseReadError(int code, const std::string& message)
    : std::runtime_error(message), code_(code) {}

bool DatabaseReadError::retryable() const {
  const int primary_code = code_ & 0xff;
  return primary_code == SQLITE_BUSY || primary_code == SQLITE_LOCKED;
}

DatabaseReader::DatabaseReader(const std::string& path) : path_(path) {
  if (path.empty() || path == ":memory:" || path.rfind("file:", 0) == 0) {
    throw std::runtime_error(
        "Database path must name a local file, not an in-memory database or "
        "SQLite URI");
  }
  struct stat status{};
  if (stat(path.c_str(), &status) < 0) {
    throw std::runtime_error("Inspect database '" + path +
                             "': " + std::strerror(errno));
  }
  if (!S_ISREG(status.st_mode)) {
    throw std::runtime_error("Database '" + path + "' must be a regular file");
  }

  try {
    int result =
        sqlite3_open_v2(path.c_str(), &db_, SQLITE_OPEN_READONLY, nullptr);
    if (result != SQLITE_OK) {
      throw DatabaseReadError(
          result, "Open database '" + path + "': " +
                      (db_ ? sqlite3_errmsg(db_) : sqlite3_errstr(result)));
    }
    result = sqlite3_busy_timeout(db_, 200);
    if (result != SQLITE_OK) {
      throw DatabaseReadError(result, "Set lock timeout for '" + path +
                                          "': " + sqlite3_errmsg(db_));
    }
  } catch (...) {
    sqlite3_close(db_);
    throw;
  }
}

DatabaseReader::~DatabaseReader() { sqlite3_close(db_); }

DatabaseSnapshot DatabaseReader::read_snapshot() {
  DatabaseSnapshot snapshot;
  const auto fail = [&](int code, const std::string& detail) {
    throw DatabaseReadError(code, "Read database '" + path_ + "': " + detail);
  };
  {
    sqlite3_stmt* raw_statement = nullptr;
    int result = sqlite3_prepare_v2(
        db_, "SELECT id, key_name, press_count FROM key_counts ORDER BY id;",
        -1, &raw_statement, nullptr);
    const std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(
        raw_statement, sqlite3_finalize);
    if (result != SQLITE_OK) {
      fail(result, sqlite3_errmsg(db_));
    }

    while ((result = sqlite3_step(statement.get())) == SQLITE_ROW) {
      if (sqlite3_column_type(statement.get(), 0) != SQLITE_INTEGER ||
          sqlite3_column_type(statement.get(), 1) != SQLITE_TEXT ||
          sqlite3_column_type(statement.get(), 2) != SQLITE_INTEGER) {
        fail(SQLITE_MISMATCH, "invalid id, key_name or press_count type");
      }
      const auto count = sqlite3_column_int64(statement.get(), 2);
      if (count < 0) {
        fail(SQLITE_MISMATCH, "press_count must be nonnegative");
      }
      const auto* name = sqlite3_column_text(statement.get(), 1);
      if (!name) {
        fail(SQLITE_NOMEM, "cannot read key_name");
      }
      const auto length = sqlite3_column_bytes(statement.get(), 1);
      snapshot.rows.push_back(
          {sqlite3_column_int64(statement.get(), 0),
           std::string(reinterpret_cast<const char*>(name), length),
           static_cast<std::uint64_t>(count)});
    }
    if (result != SQLITE_DONE) {
      fail(result, sqlite3_errmsg(db_));
    }
  }
  snapshot.read_at = std::chrono::system_clock::now();
  return snapshot;
}

}  // namespace viewer
