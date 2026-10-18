#include "Oinkster/FrameBlend.hpp"

namespace Oinkster {
namespace {
D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return barrier;
}
}

bool IsBlendableFormat(DXGI_FORMAT format) noexcept {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return true;
    default:
        return false;
    }
}

bool FrameBlendResources::Ensure(ID3D12Device* device, const D3D12_RESOURCE_DESC& backBuffer,
    D3D12_CPU_DESCRIPTOR_HANDLE srvCpu, D3D12_GPU_DESCRIPTOR_HANDLE srvGpu, UINT descriptorIncrement) {
    if (!device || backBuffer.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || backBuffer.SampleDesc.Count != 1
        || backBuffer.Width == 0 || backBuffer.Height == 0 || !IsBlendableFormat(backBuffer.Format)) {
        Release();
        return false;
    }
    if (Ready() && width_ == backBuffer.Width && height_ == backBuffer.Height && format_ == backBuffer.Format) return true;
    Release();

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = backBuffer.Width;
    description.Height = backBuffer.Height;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Format = backBuffer.Format;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    description.Flags = D3D12_RESOURCE_FLAG_NONE;

    for (std::size_t index = 0; index < HistoryCount; ++index) {
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&history_[index])))) {
            Release();
            return false;
        }
        states_[index] = D3D12_RESOURCE_STATE_COPY_DEST;
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = backBuffer.Format;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Texture2D.MipLevels = 1;
        // The game's backbuffer alpha is arbitrary (often 0). Forcing alpha
        // to 1 makes the overlay's vertex alpha the sole blend weight.
        view.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
            D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
            D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_1,
            D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_2,
            D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1);
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = srvCpu;
        cpu.ptr += static_cast<SIZE_T>(descriptorIncrement) * index;
        device->CreateShaderResourceView(history_[index].Get(), &view, cpu);
        srv_[index] = srvGpu;
        srv_[index].ptr += static_cast<UINT64>(descriptorIncrement) * index;
    }
    width_ = backBuffer.Width;
    height_ = backBuffer.Height;
    format_ = backBuffer.Format;
    current_ = 0;
    previousValid_ = false;
    return true;
}

void FrameBlendResources::Release() noexcept {
    for (auto& texture : history_) texture.Reset();
    states_ = {};
    srv_ = {};
    width_ = 0;
    height_ = 0;
    format_ = DXGI_FORMAT_UNKNOWN;
    current_ = 0;
    previousValid_ = false;
}

std::optional<D3D12_GPU_DESCRIPTOR_HANDLE> FrameBlendResources::PreviousFrame() const noexcept {
    if (!Ready() || !previousValid_) return std::nullopt;
    return srv_[(current_ + HistoryCount - 1) % HistoryCount];
}

std::optional<D3D12_GPU_DESCRIPTOR_HANDLE> FrameBlendResources::CurrentFrame() const noexcept {
    if (!Ready()) return std::nullopt;
    return srv_[current_];
}

void FrameBlendResources::RecordCapture(ID3D12GraphicsCommandList* commandList, ID3D12Resource* backBuffer) {
    if (!Ready() || !commandList || !backBuffer) {
        previousValid_ = false;
        return;
    }
    auto* target = history_[current_].Get();
    std::array<D3D12_RESOURCE_BARRIER, 2> before{
        Transition(backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE),
        Transition(target, states_[current_], D3D12_RESOURCE_STATE_COPY_DEST)};
    const UINT beforeCount = states_[current_] == D3D12_RESOURCE_STATE_COPY_DEST ? 1U : 2U;
    commandList->ResourceBarrier(beforeCount, before.data());
    commandList->CopyResource(target, backBuffer);
    std::array<D3D12_RESOURCE_BARRIER, 2> after{
        Transition(backBuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
        Transition(target, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)};
    commandList->ResourceBarrier(static_cast<UINT>(after.size()), after.data());
    states_[current_] = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    current_ = (current_ + 1) % HistoryCount;
    previousValid_ = true;
}

}
