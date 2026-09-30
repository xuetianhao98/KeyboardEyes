#include "database.h"

#include <sqlite3.h>

#include <limits>
#include <memory>
#include <stdexcept>

namespace {

void check_result(sqlite3* db, int result, const char* operation) {
  if (result != SQLITE_OK) {
    throw std::runtime_error(std::string(operation) + ": " +
                             sqlite3_errmsg(db));
  }
}

}  // namespace

Database::Database(const std::string& path) {
  try {
    const int result =
        sqlite3_open_v2(path.c_str(), &db_,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    if (result != SQLITE_OK) {
      throw std::runtime_error(
          "Open database '" + path +
          "': " + (db_ ? sqlite3_errmsg(db_) : sqlite3_errstr(result)));
    }

    constexpr const char* schema = R"sql(
      CREATE TABLE IF NOT EXISTS key_counts (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        key_name TEXT NOT NULL UNIQUE,
        press_count INTEGER NOT NULL DEFAULT 0
          CHECK (typeof(press_count) = 'integer' AND press_count >= 0)
      );
    )sql";
    check_result(db_, sqlite3_exec(db_, schema, nullptr, nullptr, nullptr),
                 "Initialize key_counts table");

    constexpr const char* save_sql = R"sql(
      INSERT INTO key_counts (key_name, press_count) VALUES (?1, ?2)
      ON CONFLICT(key_name) DO UPDATE SET press_count = excluded.press_count;
    )sql";
    check_result(
        db_, sqlite3_prepare_v2(db_, save_sql, -1, &save_statement_, nullptr),
        "Prepare count update");
  } catch (...) {
    sqlite3_finalize(save_statement_);
    sqlite3_close(db_);
    throw;
  }
}

Database::~Database() {
  sqlite3_finalize(save_statement_);
  sqlite3_close(db_);
}

std::unordered_map<std::string, std::uint64_t> Database::load_counts() {
  sqlite3_stmt* raw_statement = nullptr;
  const int result =
      sqlite3_prepare_v2(db_, "SELECT key_name, press_count FROM key_counts;",
                         -1, &raw_statement, nullptr);
  const std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(
      raw_statement, sqlite3_finalize);
  check_result(db_, result, "Prepare count query");

  std::unordered_map<std::string, std::uint64_t> counts;
  int step_result = SQLITE_OK;
  while ((step_result = sqlite3_step(statement.get())) == SQLITE_ROW) {
    if (sqlite3_column_type(statement.get(), 0) != SQLITE_TEXT ||
        sqlite3_column_type(statement.get(), 1) != SQLITE_INTEGER) {
      throw std::runtime_error("Load counts: invalid key name or count type");
    }
    const auto count = sqlite3_column_int64(statement.get(), 1);
    if (count < 0) {
      throw std::runtime_error("Load counts: count must be nonnegative");
    }
    const auto* name = sqlite3_column_text(statement.get(), 0);
    if (!name) {
      throw std::runtime_error("Load key name: " +
                               std::string(sqlite3_errmsg(db_)));
    }
    const auto length = sqlite3_column_bytes(statement.get(), 0);
    counts.emplace(std::string(reinterpret_cast<const char*>(name), length),
                   static_cast<std::uint64_t>(count));
  }
  if (step_result != SQLITE_DONE) {
    throw std::runtime_error("Load counts: " +
                             std::string(sqlite3_errmsg(db_)));
  }
  return counts;
}

void Database::save_count(const std::string& key_name,
                          std::uint64_t total_count) {
  if (total_count >
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    throw std::runtime_error("Save count: total exceeds INT64_MAX");
  }

  try {
    check_result(db_, sqlite3_clear_bindings(save_statement_),
                 "Clear count update parameters");
    check_result(
        db_,
        sqlite3_bind_text64(save_statement_, 1, key_name.data(),
                            key_name.size(), SQLITE_TRANSIENT, SQLITE_UTF8),
        "Bind key name");
    check_result(db_,
                 sqlite3_bind_int64(save_statement_, 2,
                                    static_cast<sqlite3_int64>(total_count)),
                 "Bind press count");
    if (sqlite3_step(save_statement_) != SQLITE_DONE) {
      throw std::runtime_error("Save count: " +
                               std::string(sqlite3_errmsg(db_)));
    }
  } catch (...) {
    sqlite3_reset(save_statement_);
    sqlite3_clear_bindings(save_statement_);
    throw;
  }
  check_result(db_, sqlite3_reset(save_statement_), "Reset count update");
  check_result(db_, sqlite3_clear_bindings(save_statement_),
               "Clear count update parameters");
}
