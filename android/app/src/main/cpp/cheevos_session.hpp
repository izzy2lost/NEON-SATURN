#pragma once

// RetroAchievements for the running game: ties a cheevos Client to the emulator (frames, work RAM, save states)
// and serves its HTTP through Java (CheevosHttp).

#include <memory>
#include <string>
#include <vector>

#include "cheevos_client.h"

namespace ymir::media {
struct Disc;
} // namespace ymir::media

namespace ymir::sys {
class SystemMemory;
} // namespace ymir::sys

namespace neonsaturn::cheevos {

std::unique_ptr<Backend> MakeJniBackend();

/// RetroAchievements' Saturn hash: MD5 of the first 512 bytes (volume and system header) of the disc's boot sector.
/// Empty if the disc has no Saturn header.
std::string HashDisc(const ymir::media::Disc &disc);

/// For the disc just loaded: signs in and loads its achievement set in the background; the game does not wait.
/// `memory` must outlive the session.
void BeginSession(const ymir::sys::SystemMemory &memory, const std::string &hash, const std::string &user,
                  const std::string &token);
/// After the emulator has stopped. Unlocks still in flight finish on a background thread.
void EndSession();

/// After every emulated frame, on the emulator thread with the core locked: the only time work RAM is read.
void OnFrame();

/// Achievement progress beside a save state, so loading one cannot half-trigger anything.
void AfterSaveState(const std::string &statePath);
void AfterLoadState(const std::string &statePath);

/// For the UI; fields separated by \x1f.
/// Messages to show: kind (unlock/info/problem), title, detail, points, badge URL.
std::vector<std::string> TakeMessages();
/// First row: state, game title, game badge, unlocked, total, points earned, points total.
/// Then one per achievement: unlocked, unsupported, points, badge, locked badge, title, description, progress.
std::vector<std::string> ListAchievements();

} // namespace neonsaturn::cheevos
