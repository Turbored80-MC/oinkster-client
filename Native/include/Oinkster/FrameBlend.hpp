#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>
#include <optional>

// Cosmetic frame blending (the "motion-blur" module) on Oinkster's own
// Present hook. No game memory is touched: each frame the presented
// backbuffer is copied into one of two history textures we own, and the
// overlay draws the PREVIOUS frame's copy over the current backbuffer with
// the module's blend strength.
//
// Lifetime rules:
//   * the history textures are ours - the swapchain's buffers are never
//     retained beyond the existing per-frame ComPtrs;
//   * Release() must be called (after the GPU is idle) on ResizeBuffers,
//     device loss and unload; Ensure() recreates on the next frame;
//   * any frame presented without a capture invalidates the history so a
//     stale image is never blended in after toggling, resizing or pausing.
namespace Oinkster {

class FrameBlendResources {
public:
    static constexpr std::size_t HistoryCount = 2;

    // (Re)creates the history textures when the backbuffer's size/format
    // changed. `srvCpu`/`srvGpu` are HistoryCount consecutive shader-visible
    // descriptors reserved for this class. Returns false (and stays released)
    // for formats or sample counts it cannot copy.
    bool Ensure(ID3D12Device* device, const D3D12_RESOURCE_DESC& backBuffer,
        D3D12_CPU_DESCRIPTOR_HANDLE srvCpu, D3D12_GPU_DESCRIPTOR_HANDLE srvGpu, UINT descriptorIncrement);
    void Release() noexcept;
    [[nodiscard]] bool Ready() const noexcept { return history_[0] != nullptr; }

    // GPU descriptor of the previous frame's capture, if it is valid to
    // blend this frame. Call before building the overlay draw list.
    [[nodiscard]] std::optional<D3D12_GPU_DESCRIPTOR_HANDLE> PreviousFrame() const noexcept;
    // GPU descriptor of the slot RecordCapture fills this frame: the raw
    // current frame, valid for draws recorded after the capture.
    [[nodiscard]] std::optional<D3D12_GPU_DESCRIPTOR_HANDLE> CurrentFrame() const noexcept;

    // Records: backbuffer PRESENT -> COPY_SOURCE, copy into the current
    // history slot, backbuffer -> RENDER_TARGET. Afterwards the backbuffer
    // is in RENDER_TARGET exactly as the overlay pass expects.
    void RecordCapture(ID3D12GraphicsCommandList* commandList, ID3D12Resource* backBuffer);
    // Call on every presented frame that did NOT record a capture.
    void Invalidate() noexcept { previousValid_ = false; }

private:
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, HistoryCount> history_{};
    std::array<D3D12_RESOURCE_STATES, HistoryCount> states_{};
    std::array<D3D12_GPU_DESCRIPTOR_HANDLE, HistoryCount> srv_{};
    UINT64 width_{};
    UINT height_{};
    DXGI_FORMAT format_{DXGI_FORMAT_UNKNOWN};
    std::size_t current_{};
    bool previousValid_{};
};

// True for backbuffer formats the blend can copy and sample.
[[nodiscard]] bool IsBlendableFormat(DXGI_FORMAT format) noexcept;

}
