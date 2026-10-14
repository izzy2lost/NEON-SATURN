#pragma once

#include <ymir/db/ipl_db.hpp>

#include <cstdint>
#include <filesystem>
#include <vector>

namespace neonsaturn::android {

struct IPLCandidate {
    std::filesystem::path path;
    const ymir::db::IPLROMInfo *info; ///< null for a dump that is not in the database
};

/// Hashes the file as Ymir does and looks it up in the IPL ROM database. Null if unknown or not an IPL ROM.
const ymir::db::IPLROMInfo *IdentifyIPL(const std::filesystem::path &path);

/// Every IPL-sized file in the directory, identified.
std::vector<IPLCandidate> ScanIPLDirectory(const std::filesystem::path &directory);

/// The best IPL ROM for an SMPC area code, the way the desktop frontend picks: one made for the region, then a
/// region-free one, then any known one. Null if no candidate is in the database.
const IPLCandidate *SelectIPL(const std::vector<IPLCandidate> &candidates, uint8_t areaCode);

} // namespace neonsaturn::android
