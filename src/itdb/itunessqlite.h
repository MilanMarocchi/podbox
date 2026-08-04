#pragma once

#include "itdb/itunesdb.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace podbox {

// nano 6G/7G firmware reads this SQLite bundle in addition to iTunesCDB.
// PodBox stages a copy of the device-initialized bundle so model-specific
// schema additions, indexes and triggers are retained.
bool writeItunesSqliteBundle(
    const Library& library,
    const std::filesystem::path& existingDirectory,
    const std::filesystem::path& outputDirectory,
    const std::vector<std::uint8_t>& uuid,
    const std::vector<std::uint8_t>& nonce,
    std::string* error);

// Checks schemas, row counts, SQLite integrity, and the hashAB-signed
// Locations.itdb.cbk. Used both before enabling writes and after staging.
bool validateItunesSqliteBundle(
    const Library& library,
    const std::filesystem::path& directory,
    const std::vector<std::uint8_t>& uuid,
    const std::vector<std::uint8_t>& nonce,
    std::string* error);

}  // namespace podbox
