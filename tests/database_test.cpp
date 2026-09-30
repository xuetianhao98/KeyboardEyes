#include "database.h"

#include <sqlite3.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

class TemporaryDatabase {
 public:
  TemporaryDatabase() {
    std::string pattern =
        (std::filesystem::temp_directory_path() / "keyboardeyes-test-XXXXXX")
            .string();
    const char* directory = mkdtemp(pattern.data());
    require(directory != nullptr, "Create temporary directory failed");
    directory_ = directory;
  }

  ~TemporaryDatabase() {
    std::error_code ignored;
    std::filesystem::remove_all(directory_, ignored);
  }

  std::string path() const { return (directory_ / "stats.db").string(); }

 private:
  std::filesystem::path directory_;
};

using Rows = std::vector<std::vector<std::string>>;

// Query through a separate connection to verify committed database contents.
Rows query(const std::string& path, const char* sql) {
  sqlite3* raw_db = nullptr;
  const int open_result =
      sqlite3_open_v2(path.c_str(), &raw_db, SQLITE_OPEN_READONLY, nullptr);
  const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw_db,
                                                              sqlite3_close);
  require(open_result == SQLITE_OK, "Open inspection connection failed");

  sqlite3_stmt* raw_statement = nullptr;
  const int prepare_result =
      sqlite3_prepare_v2(db.get(), sql, -1, &raw_statement, nullptr);
  const std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(
      raw_statement, sqlite3_finalize);
  require(prepare_result == SQLITE_OK, sqlite3_errmsg(db.get()));

  Rows rows;
  int result = SQLITE_OK;
  while ((result = sqlite3_step(statement.get())) == SQLITE_ROW) {
    std::vector<std::string> row;
    for (int column = 0; column < sqlite3_column_count(statement.get());
         ++column) {
      const auto* value = sqlite3_column_text(statement.get(), column);
      row.emplace_back(value ? reinterpret_cast<const char*>(value) : "");
    }
    rows.push_back(std::move(row));
  }
  require(result == SQLITE_DONE, sqlite3_errmsg(db.get()));
  return rows;
}

void test_create_table(const std::string& path) {
  Database database(path);
  require(query(path,
                "SELECT name, type, pk FROM pragma_table_info('key_counts') "
                "ORDER BY cid;") == Rows{{"id", "INTEGER", "1"},
                                         {"key_name", "TEXT", "0"},
                                         {"press_count", "INTEGER", "0"}},
          "Table columns or primary key do not match");
  const auto schema = query(path,
                            "SELECT sql FROM sqlite_master WHERE type='table' "
                            "AND name='key_counts';");
  require(schema.size() == 1 &&
              schema[0][0].find("AUTOINCREMENT") != std::string::npos,
          "ID must use AUTOINCREMENT");
  require(
      query(
          path,
          "SELECT columns.name FROM pragma_index_list('key_counts') AS indexes "
          "JOIN pragma_index_info(indexes.name) AS columns "
          "WHERE indexes.\"unique\" = 1;") == Rows{{"key_name"}},
      "Key name must have a unique index");
}

void test_query_update(const std::string& path) {
  Database database(path);
  database.save_count("KEY_A", 3);
  database.save_count("KEY_ENTER", 5);
  require(
      query(
          path,
          "SELECT key_name, press_count FROM key_counts ORDER BY key_name;") ==
          Rows{{"KEY_A", "3"}, {"KEY_ENTER", "5"}},
      "Inserted counts do not match");
  const auto original_id =
      query(path, "SELECT id FROM key_counts WHERE key_name='KEY_A';");

  database.save_count("KEY_A", 8);
  require(
      query(
          path,
          "SELECT key_name, press_count FROM key_counts ORDER BY key_name;") ==
          Rows{{"KEY_A", "8"}, {"KEY_ENTER", "5"}},
      "Update must change the existing count without adding a row");
  require(query(path, "SELECT id FROM key_counts WHERE key_name='KEY_A';") ==
              original_id,
          "Update must preserve the existing ID");
}

void test_load_counts(const std::string& path) {
  const std::unordered_map<std::string, std::uint64_t> expected{
      {"KEY_A", 12}, {"KEY_ENTER", 7}, {"KEY_SPACE", 25}};
  {
    Database database(path);
    for (const auto& [name, count] : expected) {
      database.save_count(name, count);
    }
  }
  Database reopened(path);
  require(reopened.load_counts() == expected,
          "Reloaded counts do not match the saved data");
}

}  // namespace

int main(int argc, char* argv[]) {
  try {
    require(argc == 2, "Expected one test name");
    TemporaryDatabase temporary;
    const std::string test_name = argv[1];
    if (test_name == "create_table") {
      test_create_table(temporary.path());
    } else if (test_name == "query_update") {
      test_query_update(temporary.path());
    } else if (test_name == "load_counts") {
      test_load_counts(temporary.path());
    } else {
      throw std::runtime_error("Unknown test: " + test_name);
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
