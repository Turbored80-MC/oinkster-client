#pragma once

#include <d3d12.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>

// One-shot PNG capture using the overlay's own command list. Requests are
// bounded and consumed only at the requested stage (raw game frame or the
// fully composed frame). GPU readback is collected on the render thread;
// PNG encoding and disk I/O run on a worker thread.
namespace Oinkster::ScreenshotCapture {

struct Result {
    std::uint64_t revision{};
    std::filesystem::path file;
    std::string error;
};

[[nodiscard]] bool Request(const std::filesystem::path& file, bool includeHud, std::string& error);
[[nodiscard]] bool NeedsPresentWork() noexcept;
[[nodiscard]] Result CurrentResult();
void Shutdown() noexcept;

[[nodiscard]] std::filesystem::path ScreenshotDirectory();
[[nodiscard]] std::filesystem::path NextScreenshotFile();

class GpuCapture {
public:
    void Collect(UINT64 completedFence);
    void Record(ID3D12Device* device, ID3D12GraphicsCommandList* commandList,
        ID3D12Resource* backBuffer, bool includeHud);
    void Submitted(UINT64 fenceValue) noexcept;
    void Release() noexcept;

private:
    struct Slot {
        Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        std::filesystem::path file;
        UINT width{};
        UINT height{};
        DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
        UINT64 fence{};
        bool pending{};
        bool recorded{};
    };
    static constexpr std::size_t SlotCount = 2;
    std::array<Slot, SlotCount> slots_{};
};

}
