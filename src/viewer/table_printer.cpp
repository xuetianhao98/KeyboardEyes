#include "table_printer.h"

#include <algorithm>
#include <array>
#include <ctime>
#include <iomanip>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

namespace viewer {
namespace {

// Keep unusual names intact without allowing embedded controls to break rows.
std::string escape_name(const std::string& name) {
  constexpr char hex[] = "0123456789ABCDEF";
  std::string escaped;
  for (unsigned char character : name) {
    if (character == '\\') {
      escaped += "\\\\";
    } else if (character < 0x20 || character == 0x7f || character == '|') {
      escaped += "\\x";
      escaped += hex[character >> 4];
      escaped += hex[character & 0xf];
    } else {
      escaped += static_cast<char>(character);
    }
  }
  return escaped;
}

}  // namespace

void print_snapshot(const DatabaseSnapshot& snapshot, std::ostream& output) {
  using Cells = std::array<std::string, 3>;
  const Cells headers{"id", "key_name", "press_count"};
  std::array<std::size_t, 3> widths{headers[0].size(), headers[1].size(),
                                    headers[2].size()};
  std::vector<Cells> rows;
  rows.reserve(snapshot.rows.size());
  for (const auto& row : snapshot.rows) {
    rows.push_back({std::to_string(row.id), escape_name(row.key_name),
                    std::to_string(row.press_count)});
    for (std::size_t column = 0; column < widths.size(); ++column) {
      widths[column] = std::max(widths[column], rows.back()[column].size());
    }
  }

  const std::time_t read_at =
      std::chrono::system_clock::to_time_t(snapshot.read_at);
  std::tm utc{};
  if (!gmtime_r(&read_at, &utc)) {
    throw std::runtime_error("Format snapshot timestamp failed");
  }
  std::ostringstream text;
  text << "Table: key_counts | Read at: "
       << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ") << '\n';
  const auto print_row = [&](const Cells& cells) {
    text << "| ";
    for (std::size_t column = 0; column < cells.size(); ++column) {
      text << cells[column]
           << std::string(widths[column] - cells[column].size(), ' ') << " |";
      if (column + 1 != cells.size()) {
        text << ' ';
      }
    }
    text << '\n';
  };
  print_row(headers);
  for (const auto& row : rows) {
    print_row(row);
  }
  text << "Rows: " << rows.size() << "\n\n";
  output << text.str();
  output.flush();
  if (!output) {
    throw std::runtime_error("Write snapshot to stdout failed");
  }
}

}  // namespace viewer
