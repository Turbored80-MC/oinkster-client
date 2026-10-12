#pragma once

#include <string>

// ---------------------------------------------------------------------------
// Unload safety.
//
// Teardown is best-effort: a hook trampoline can still have a live thread
// inside it, and un-subclassing a window owned by another thread can fail
// outright. In those cases our code is still reachable from the game, so
// calling FreeLibrary would unmap it and turn the next call into a jump into
// unmapped memory.
//
// Any teardown path that cannot fully detach calls Block() with a reason. The
// bootstrap thread then exits WITHOUT unloading the module - leaking the DLL
// image for the remainder of the Minecraft session, which is strictly
// preferable to crashing the game. The reason is logged so the leak is
// visible in oinkster-native.log rather than silent.
// ---------------------------------------------------------------------------
namespace Oinkster::UnloadGuard {

// Marks the module as unsafe to unload. Idempotent; the first reason wins.
void Block(std::string reason) noexcept;

[[nodiscard]] bool Blocked() noexcept;

// The reason recorded by the first Block() call, or an empty string.
[[nodiscard]] std::string Reason();

// Test-only: returns the guard to its initial state.
void ResetForTesting() noexcept;

}
