#pragma once

#include "duckdb_extension.h"

namespace blobhttp {
void RegisterHttpFunctions(duckdb_connection connection);
} // namespace blobhttp
