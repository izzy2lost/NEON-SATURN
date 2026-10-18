// Hashes a disc's first track the way Redump dumps it, so the launcher can identify a game whatever its file is named.

#include "disc_hash.hpp"

#include <libchdr/cdrom.h>
#include <libchdr/chd.h>
#include <zlib.h>

#include <cstring>
#include <memory>
#include <vector>

namespace neonsaturn::android {

namespace {

struct ChdCloser {
    void operator()(chd_file *chd) const {
        chd_close(chd);
    }
};

struct Track1Info {
    char type[32] = {};
    uint32_t frames = 0; // frames stored in the file, a stored pregap included (Redump's bins start at INDEX 00 too)
};

std::optional<Track1Info> ReadTrack1Info(chd_file *chd) {
    char meta[256];
    uint32_t metaLength = 0;
    Track1Info info{};
    int track = 0;
    int frames = 0;
    char subtype[32] = {};

    // CHT2 (current chdman) and CHTR (old chdman); track 1 is always the first entry
    if (chd_get_metadata(chd, CDROM_TRACK_METADATA2_TAG, 0, meta, sizeof(meta) - 1, &metaLength, nullptr, nullptr) ==
        CHDERR_NONE) {
        meta[metaLength] = '\0';
        if (std::sscanf(meta, "TRACK:%d TYPE:%31s SUBTYPE:%31s FRAMES:%d", &track, info.type, subtype, &frames) < 4) {
            return std::nullopt;
        }
    } else if (chd_get_metadata(chd, CDROM_TRACK_METADATA_TAG, 0, meta, sizeof(meta) - 1, &metaLength, nullptr,
                                nullptr) == CHDERR_NONE) {
        meta[metaLength] = '\0';
        if (std::sscanf(meta, "TRACK:%d TYPE:%31s SUBTYPE:%31s FRAMES:%d", &track, info.type, subtype, &frames) < 4) {
            return std::nullopt;
        }
    } else {
        return std::nullopt;
    }

    if (track != 1 || frames <= 0) {
        return std::nullopt;
    }
    info.frames = static_cast<uint32_t>(frames);
    return info;
}

} // namespace

std::optional<TrackHash> HashChdTrack1(FILE *file) {
    chd_file *rawChd = nullptr;
    if (chd_open_file(file, CHD_OPEN_READ, nullptr, &rawChd) != CHDERR_NONE) {
        return std::nullopt;
    }
    std::unique_ptr<chd_file, ChdCloser> chd{rawChd};

    const chd_header *header = chd_get_header(chd.get());
    if (header == nullptr || header->unitbytes != CD_FRAME_SIZE || header->hunkbytes % CD_FRAME_SIZE != 0) {
        return std::nullopt;
    }

    const auto info = ReadTrack1Info(chd.get());
    if (!info) {
        return std::nullopt;
    }
    // Only raw sectors match Redump's bins; cooked 2048-byte tracks would need their headers and ECC rebuilt
    const bool audio = std::strcmp(info->type, "AUDIO") == 0;
    if (!audio && std::strcmp(info->type, "MODE1_RAW") != 0 && std::strcmp(info->type, "MODE2_RAW") != 0) {
        return std::nullopt;
    }

    // Track 1 starts at the first frame of the file
    const uint32_t framesPerHunk = header->hunkbytes / CD_FRAME_SIZE;
    std::vector<uint8_t> hunk(header->hunkbytes);
    uLong crc = crc32(0L, Z_NULL, 0);
    uint32_t cachedHunk = UINT32_MAX;

    for (uint32_t frame = 0; frame < info->frames; frame++) {
        const uint32_t hunkIndex = frame / framesPerHunk;
        if (hunkIndex != cachedHunk) {
            if (hunkIndex >= header->hunkcount || chd_read(chd.get(), hunkIndex, hunk.data()) != CHDERR_NONE) {
                return std::nullopt;
            }
            cachedHunk = hunkIndex;
        }
        uint8_t *sector = &hunk[(frame % framesPerHunk) * CD_FRAME_SIZE];
        if (audio) {
            // chdman stores audio big-endian; Redump's bins are little-endian
            for (uint32_t i = 0; i < CD_MAX_SECTOR_DATA; i += 2) {
                std::swap(sector[i], sector[i + 1]);
            }
        }
        crc = crc32(crc, sector, CD_MAX_SECTOR_DATA);
    }

    return TrackHash{static_cast<uint32_t>(crc), static_cast<uint64_t>(info->frames) * CD_MAX_SECTOR_DATA};
}

} // namespace neonsaturn::android
