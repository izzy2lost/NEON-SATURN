#pragma once

#include <cstdint>
#include <cstdio>
#include <optional>

namespace neonsaturn::android {

struct TrackHash {
    uint32_t crc32;
    uint64_t size;
};

/// CRC32 and size of the first track of a CD CHD, as raw 2352-byte sectors, which is what Redump's
/// "(Track 1).bin" holds. Empty if the file isn't a CD CHD or track 1 isn't stored raw.
std::optional<TrackHash> HashChdTrack1(FILE *file);

} // namespace neonsaturn::android
