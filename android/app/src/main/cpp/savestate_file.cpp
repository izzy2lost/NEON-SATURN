#include "savestate_file.hpp"

#include <cereal/archives/portable_binary.hpp>
#include <serdes/cereal_savestate.hpp>

#include <SDL3/SDL_log.h>

#include <array>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace neonsaturn::android {

namespace {

// "NSST", header version, layout revision; all little-endian. Older builds wrote the cereal archive alone.
constexpr std::array<char, 4> kMagic{'N', 'S', 'S', 'T'};
constexpr uint32_t kHeaderVersion = 1;
constexpr std::size_t kHeaderSize = kMagic.size() + sizeof(uint32_t) * 2;

void WriteLE32(std::ostream &out, uint32_t value) {
    const std::array<char, 4> bytes{static_cast<char>(value), static_cast<char>(value >> 8),
                                    static_cast<char>(value >> 16), static_cast<char>(value >> 24)};
    out.write(bytes.data(), bytes.size());
}

uint32_t ReadLE32(const char *data) {
    const auto *bytes = reinterpret_cast<const unsigned char *>(data);
    return bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
}

void ParseState(const std::string &payload, uint32_t revision, ymir::savestate::SaveState &state) {
    ymir::savestate::ScopedPortLayoutRevision scopedRevision{revision};
    std::istringstream in{payload};
    cereal::PortableBinaryInputArchive archive{in};
    archive(state);
}

} // namespace

void WriteSaveStateFile(std::ostream &out, const ymir::savestate::SaveState &state) {
    out.write(kMagic.data(), kMagic.size());
    WriteLE32(out, kHeaderVersion);
    WriteLE32(out, ymir::savestate::kPortLayoutRevision);
    cereal::PortableBinaryOutputArchive archive{out};
    archive(state);
}

void ReadSaveStateFile(const std::filesystem::path &path, ymir::savestate::SaveState &state) {
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        throw std::runtime_error("cannot open file");
    }
    std::string data{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};

    uint32_t revision = 0;
    std::string payload;
    if (data.size() >= kHeaderSize && std::memcmp(data.data(), kMagic.data(), kMagic.size()) == 0) {
        const uint32_t headerVersion = ReadLE32(data.data() + 4);
        if (headerVersion > kHeaderVersion) {
            throw std::runtime_error("made by a newer version of the app");
        }
        revision = ReadLE32(data.data() + 8);
        if (revision > ymir::savestate::kPortLayoutRevision) {
            throw std::runtime_error("made by a newer version of the app");
        }
        payload = data.substr(kHeaderSize);
    } else {
        payload = std::move(data);
    }

    // The recorded layout first, then the rest from newest to oldest
    std::vector<uint32_t> attempts{revision};
    for (uint32_t r = ymir::savestate::kPortLayoutRevision + 1; r-- > 0;) {
        if (r != revision) {
            attempts.push_back(r);
        }
    }

    std::string firstError;
    for (const uint32_t attempt : attempts) {
        try {
            // Every field is assigned by the serializer, so a failed attempt leaves nothing behind for the next one
            ParseState(payload, attempt, state);
            if (attempt != revision) {
                SDL_Log("Save state %s parsed as layout revision %u instead of %u", path.c_str(), attempt, revision);
            }
            return;
        } catch (const std::exception &e) {
            if (firstError.empty()) {
                firstError = e.what();
            }
        }
    }
    throw std::runtime_error(firstError.empty() ? "unreadable save state" : firstError);
}

} // namespace neonsaturn::android
