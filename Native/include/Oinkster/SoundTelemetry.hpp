#pragma once

#include <string>

namespace Oinkster { class ModuleManager; }

namespace Oinkster::SoundTelemetry {

// Capability::SoundEvents. Hooks the client's world-sound entry point - the
// LevelRenderer helper every LevelListener sound and level event goes through
// with (const std::string& name, const Vec3& position, float volume, ...),
// resolved by a unique .text signature - and publishes a SoundEvent per sound
// the game is about to play, after the game's own distance cull. Rate-limited;
// Pending until the first plausible sound.
[[nodiscard]] bool Install(ModuleManager& modules, std::wstring& detail) noexcept;
void Remove() noexcept;

}
