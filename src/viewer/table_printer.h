#pragma once

#include <iosfwd>

#include "database_reader.h"

namespace viewer {

void print_snapshot(const DatabaseSnapshot& snapshot, std::ostream& output);

}  // namespace viewer
