#pragma once

#include "Oinkster/Events.hpp"
#include <string>

namespace Oinkster { class ModuleManager; }

namespace Oinkster::SessionTelemetry {

// Connection latency and world identity for Capability::NetworkMetrics and
// Capability::ServerState.
//
// Install hooks RakNet::RakPeer::Receive (resolved by a unique .text
// signature). The game drains every peer it owns through Receive each network
// tick, so the detour samples the peer while it is guaranteed alive: when the
// peer accepts no incoming connections and has exactly one active remote
// system, that remote is the server the client is connected to, and its ping
// ring and address class (never the address itself) are stored in atomics.
// A hosting peer (one that accepts incoming connections) only marks the
// session as locally hosted.
//
// Observe runs on the render thread from WorldTelemetry's _renderLevelPrep
// hook and publishes NetworkMetrics and ServerState about once a second.
// Both capabilities are Pending until their first plausible read.
[[nodiscard]] bool Install(ModuleManager& modules, std::wstring& detail) noexcept;
void Remove() noexcept;

// Render thread. `actor` is the local render player _renderLevelPrep passes.
void Observe(const void* actor, SteadyTime now) noexcept;

}
