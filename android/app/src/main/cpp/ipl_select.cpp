#include "ipl_select.hpp"

#include <ymir/core/hash.hpp>
#include <ymir/sys/memory_defs.hpp>

#include <algorithm>
#include <fstream>
#include <string_view>
#include <tuple>

namespace neonsaturn::android {

namespace {

ymir::db::SystemRegion RegionForAreaCode(uint8_t areaCode) {
    // SMPC area codes:
    //   0x1  J  Japan
    //   0x2  T  Asia NTSC      0x6  K  Korea
    //   0x4  U  North America  0x5  B  Brazil
    //   0xC  E  PAL            0xA  A  Asia PAL   0xD  L  Latin America PAL
    switch (areaCode) {
    case 0x1: return ymir::db::SystemRegion::JP;
    case 0x2: [[fallthrough]];
    case 0x6: return ymir::db::SystemRegion::KR;
    case 0x4: [[fallthrough]];
    case 0x5: [[fallthrough]];
    case 0xA: [[fallthrough]];
    case 0xC: [[fallthrough]];
    case 0xD: return ymir::db::SystemRegion::US_EU;
    default: return ymir::db::SystemRegion::None;
    }
}

// Among equally good matches, an unpatched ROM over a region-free one, a retail Saturn over HiSaturn/V-Saturn/dev
// kits, then the newest revision
auto Preference(const ymir::db::IPLROMInfo &info) {
    const bool retail = info.variant == ymir::db::SystemVariant::Saturn;
    // 1.00a and 1.01a share a date in the database, so the version string breaks the tie
    return std::make_tuple(!info.regionFree, retail, info.year, info.month, info.day, std::string_view{info.version});
}

} // namespace

const ymir::db::IPLROMInfo *IdentifyIPL(const std::filesystem::path &path) {
    std::error_code error{};
    if (std::filesystem::file_size(path, error) != ymir::sys::kIPLSize || error) {
        return nullptr;
    }

    std::vector<char> buffer(ymir::sys::kIPLSize);
    std::ifstream in{path, std::ios::binary};
    if (!in.read(buffer.data(), buffer.size())) {
        return nullptr;
    }
    return ymir::db::GetIPLROMInfo(ymir::CalcHash128(buffer.data(), buffer.size(), ymir::sys::kIPLHashSeed));
}

std::vector<IPLCandidate> ScanIPLDirectory(const std::filesystem::path &directory) {
    std::vector<IPLCandidate> candidates;
    std::error_code error{};
    for (const auto &entry : std::filesystem::directory_iterator{directory, error}) {
        if (!entry.is_regular_file(error)) {
            continue;
        }
        candidates.push_back({entry.path(), IdentifyIPL(entry.path())});
    }
    // Directory order is arbitrary; sort so ties always resolve the same way
    std::sort(candidates.begin(), candidates.end(),
              [](const IPLCandidate &lhs, const IPLCandidate &rhs) { return lhs.path < rhs.path; });
    return candidates;
}

const IPLCandidate *SelectIPL(const std::vector<IPLCandidate> &candidates, uint8_t areaCode) {
    const auto region = RegionForAreaCode(areaCode);

    const IPLCandidate *best = nullptr;
    int bestTier = 0;
    for (const auto &candidate : candidates) {
        if (candidate.info == nullptr) {
            continue;
        }
        const int tier = candidate.info->region == region ? 3 : candidate.info->regionFree ? 2 : 1;
        if (best == nullptr || tier > bestTier ||
            (tier == bestTier && Preference(*candidate.info) > Preference(*best->info))) {
            best = &candidate;
            bestTier = tier;
        }
    }
    return best;
}

} // namespace neonsaturn::android
