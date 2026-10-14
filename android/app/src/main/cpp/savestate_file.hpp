#pragma once

#include <ymir/savestate/savestate.hpp>

#include <filesystem>
#include <iosfwd>
#include <string>

namespace neonsaturn::android {

/// Writes a save state slot file: a small port header recording the serializer layout revision, then the state.
void WriteSaveStateFile(std::ostream &out, const ymir::savestate::SaveState &state);

/// Reads a save state slot file written by any build of the port. Files without the header (older builds) are read
/// with the layout those builds wrote, and if the recorded layout does not parse the other known layouts are tried, so
/// one misattributed revision does not lose the state. Throws if no layout parses.
void ReadSaveStateFile(const std::filesystem::path &path, ymir::savestate::SaveState &state);

} // namespace neonsaturn::android
