#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>

#include <d3d12.h>
#include <wrl/client.h>

// Records the game's own frames (as Minecraft drew them, before any Oinkster
// overlay) to an H.264 .mp4 with Media Foundation. The render thread copies
// each back buffer into a small ring of readback buffers (GpuCapture); once
// the GPU has finished a copy its pixels go to an encoder thread, which
// writes them with real-time timestamps. Frames are dropped, never queued
// without bound, when the encoder falls behind.
namespace Oinkster::VideoRecorder {

using Clock = std::chrono::steady_clock;

struct State {
    bool recording{};
    // Stopped, and the file is being finished.
    bool finishing{};
    std::uint64_t frames{};
    std::uint64_t dropped{};
    double seconds{};
    std::filesystem::path file;
    std::string error;
};

// Starts a recording into `file` (its folder is created). False with `error`
// if one is already running.
[[nodiscard]] bool Start(const std::filesystem::path& file, std::string& error);
// Stops capturing; the encoder finishes the file on its own thread.
void Stop() noexcept;
[[nodiscard]] bool Recording() noexcept;
[[nodiscard]] State Current();
// Waits for the encoder thread (DLL unload).
void Shutdown() noexcept;

// Encodes `count` frames of a moving gradient straight to `file` through the
// same writer a recording uses (tests: proves this machine can write H.264).
[[nodiscard]] bool WriteTestVideo(const std::filesystem::path& file, UINT width, UINT height, int count, std::string& error);

// The render thread's side. Not thread-safe; one instance, render thread only.
class GpuCapture {
public:
    // Hands every copy the GPU has finished (fence value <= completed) to the
    // encoder.
    void Collect(UINT64 completedFence);
    // Records a copy of `backBuffer` (in PRESENT state, left in PRESENT) into
    // a free readback buffer, if a recording is running and one is free.
    void Record(ID3D12Device* device, ID3D12GraphicsCommandList* commandList, ID3D12Resource* backBuffer, Clock::time_point at);
    // The fence value the command list holding the last Record was signalled with.
    void Submitted(UINT64 fenceValue) noexcept;
    // Drops the GPU buffers (device loss, resize, unload).
    void Release() noexcept;

private:
    struct Slot {
        Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
        UINT64 fence{};
        bool pending{};
        bool recorded{};
        Clock::time_point at{};
    };
    static constexpr std::size_t SlotCount = 4;
    Slot slots_[SlotCount];
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint_{};
    UINT width_{};
    UINT height_{};
    DXGI_FORMAT format_{DXGI_FORMAT_UNKNOWN};
    bool ready_{};
};

}
