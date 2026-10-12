#include "Oinkster/Dx12Overlay.hpp"
#include "Oinkster/FrameBlend.hpp"
#include "Oinkster/ModuleManager.hpp"
#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using Microsoft::WRL::ComPtr;

void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }

D3D12_RESOURCE_DESC TextureDesc(UINT64 width, UINT height, DXGI_FORMAT format) {
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = width;
    description.Height = height;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Format = format;
    description.SampleDesc.Count = 1;
    description.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    return description;
}

// Exercises the frame-blend resource lifecycle on a WARP device: no GPU and
// no Minecraft needed, but real D3D12 resource creation, state transitions,
// copies and fences.
void FrameBlendLifecycleOnWarp() {
    ComPtr<IDXGIFactory4> factory;
    Require(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))), "DXGI factory");
    ComPtr<IDXGIAdapter> warp;
    Require(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))), "WARP adapter");
    ComPtr<ID3D12Device> device;
    Require(SUCCEEDED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))), "WARP device");

    D3D12_DESCRIPTOR_HEAP_DESC heapDescription{};
    heapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDescription.NumDescriptors = Oinkster::FrameBlendResources::HistoryCount;
    heapDescription.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> heap;
    Require(SUCCEEDED(device->CreateDescriptorHeap(&heapDescription, IID_PPV_ARGS(&heap))), "SRV heap");
    const UINT increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    Oinkster::FrameBlendResources blend;
    const auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
    const auto gpu = heap->GetGPUDescriptorHandleForHeapStart();
    Require(!blend.Ready() && !blend.PreviousFrame(), "Starts released");
    Require(!blend.Ensure(device.Get(), TextureDesc(64, 64, DXGI_FORMAT_R32G32B32A32_FLOAT), cpu, gpu, increment) && !blend.Ready(),
        "Unsupported formats are refused and leave nothing allocated");
    auto msaa = TextureDesc(64, 64, DXGI_FORMAT_R8G8B8A8_UNORM);
    msaa.SampleDesc.Count = 4;
    Require(!blend.Ensure(device.Get(), msaa, cpu, gpu, increment), "Multisampled backbuffers are refused");
    Require(blend.Ensure(device.Get(), TextureDesc(64, 64, DXGI_FORMAT_R8G8B8A8_UNORM), cpu, gpu, increment) && blend.Ready(), "Creates");
    Require(!blend.PreviousFrame(), "No previous frame before the first capture");

    // Stand-in backbuffer, created in PRESENT (== COMMON) like a swapchain buffer.
    D3D12_HEAP_PROPERTIES heapProperties{};
    heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;
    const auto backBufferDesc = TextureDesc(64, 64, DXGI_FORMAT_R8G8B8A8_UNORM);
    ComPtr<ID3D12Resource> backBuffer;
    Require(SUCCEEDED(device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &backBufferDesc,
        D3D12_RESOURCE_STATE_PRESENT, nullptr, IID_PPV_ARGS(&backBuffer))), "Backbuffer stand-in");

    D3D12_COMMAND_QUEUE_DESC queueDescription{};
    queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    Require(SUCCEEDED(device->CreateCommandQueue(&queueDescription, IID_PPV_ARGS(&queue))), "Queue");
    ComPtr<ID3D12CommandAllocator> allocator;
    Require(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))), "Allocator");
    ComPtr<ID3D12GraphicsCommandList> list;
    Require(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list))), "List");
    ComPtr<ID3D12Fence> fence;
    Require(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))), "Fence");
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Require(event != nullptr, "Fence event");

    UINT64 fenceValue = 0;
    const auto submitAndWait = [&] {
        // Return the backbuffer to PRESENT, as the overlay pass does.
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = backBuffer.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1, &barrier);
        Require(SUCCEEDED(list->Close()), "Command list closes (valid barriers and copies)");
        ID3D12CommandList* lists[]{list.Get()};
        queue->ExecuteCommandLists(1, lists);
        Require(SUCCEEDED(queue->Signal(fence.Get(), ++fenceValue)), "Signal");
        if (fence->GetCompletedValue() < fenceValue) {
            Require(SUCCEEDED(fence->SetEventOnCompletion(fenceValue, event)), "Fence wait");
            Require(WaitForSingleObject(event, 5000) == WAIT_OBJECT_0, "GPU completes");
        }
        Require(SUCCEEDED(allocator->Reset()) && SUCCEEDED(list->Reset(allocator.Get(), nullptr)), "Reset for next frame");
    };

    for (int frame = 0; frame < 4; ++frame) {
        blend.RecordCapture(list.Get(), backBuffer.Get());
        submitAndWait();
        Require(blend.PreviousFrame().has_value(), "A capture makes the previous frame available");
    }
    const auto first = blend.PreviousFrame()->ptr;
    blend.RecordCapture(list.Get(), backBuffer.Get());
    submitAndWait();
    Require(blend.PreviousFrame()->ptr != first, "History ping-pongs between two textures");
    blend.Invalidate();
    Require(!blend.PreviousFrame(), "A frame without a capture invalidates history");
    Require(device->GetDeviceRemovedReason() == S_OK, "No device removal after captures");

    Require(blend.Ensure(device.Get(), TextureDesc(64, 64, DXGI_FORMAT_R8G8B8A8_UNORM), cpu, gpu, increment) && !blend.PreviousFrame(),
        "Reusing the same size keeps resources but history stays invalid");
    Require(blend.Ensure(device.Get(), TextureDesc(128, 72, DXGI_FORMAT_B8G8R8A8_UNORM), cpu, gpu, increment) && blend.Ready(),
        "A resize recreates the history at the new size and format");
    blend.Release();
    blend.Release();
    Require(!blend.Ready() && !blend.PreviousFrame(), "Release is idempotent and complete");
    CloseHandle(event);
}

void OverlayInstallIsRepeatable() {
    for (int attempt = 0; attempt < 2; ++attempt) {
        std::wstring error;
        Oinkster::ModuleManager modules;
        for (auto& module : Oinkster::CreateModuleCatalog()) modules.Register(std::move(module));
        if (!Oinkster::Dx12Overlay::Install(modules, error)) throw std::runtime_error("Install failed");
        Require(Oinkster::Dx12Overlay::Install(modules, error), "A second Install is a no-op success");
        Oinkster::Dx12Overlay::Remove();
        Oinkster::Dx12Overlay::Remove();
    }
}
}

int wmain() {
    try {
        OverlayInstallIsRepeatable();
        FrameBlendLifecycleOnWarp();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::wcout << L"D3D12 hook install/removal and frame-blend lifecycle passed\n";
    return 0;
}
