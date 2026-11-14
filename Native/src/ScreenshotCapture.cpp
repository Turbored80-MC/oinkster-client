#include "Oinkster/ScreenshotCapture.hpp"

#include "Oinkster/Diagnostics.hpp"
#include "Oinkster/Modules/PacketLogic.hpp"
#include <Windows.h>
#include <ShlObj.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <format>
#include <fstream>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace Oinkster::ScreenshotCapture {
namespace {

struct PendingRequest {
    std::filesystem::path file;
    bool includeHud{};
};

struct CpuFrame {
    std::filesystem::path file;
    UINT width{};
    UINT height{};
    std::vector<std::uint8_t> rgba;
};

constexpr std::size_t MaxRequests = 2;
constexpr std::size_t MaxFrames = 2;
std::mutex mutex;
std::condition_variable wake;
std::deque<PendingRequest> requests;
std::deque<CpuFrame> frames;
std::jthread worker;
bool stopping{};
bool workerBusy{};
std::size_t gpuPending{};
Result result;

bool Supported(DXGI_FORMAT format) noexcept {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return true;
    default:
        return false;
    }
}

void ConvertPixel(const std::uint8_t* source, std::uint8_t* target, DXGI_FORMAT format) noexcept {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        target[0] = source[0]; target[1] = source[1]; target[2] = source[2]; target[3] = 255;
        break;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        target[0] = source[2]; target[1] = source[1]; target[2] = source[0]; target[3] = 255;
        break;
    case DXGI_FORMAT_R10G10B10A2_UNORM: {
        std::uint32_t value{};
        std::memcpy(&value, source, sizeof(value));
        target[0] = static_cast<std::uint8_t>((value & 0x3FFU) >> 2U);
        target[1] = static_cast<std::uint8_t>(((value >> 10U) & 0x3FFU) >> 2U);
        target[2] = static_cast<std::uint8_t>(((value >> 20U) & 0x3FFU) >> 2U);
        target[3] = 255;
        break;
    }
    default:
        target[0] = target[1] = target[2] = 0; target[3] = 255;
        break;
    }
}

void SetResult(const std::filesystem::path& file, std::string error) {
    std::scoped_lock lock(mutex);
    result.file = file;
    result.error = std::move(error);
    ++result.revision;
}

void RunWorker() {
    for (;;) {
        CpuFrame frame;
        {
            std::unique_lock lock(mutex);
            wake.wait(lock, [] { return stopping || !frames.empty(); });
            if (frames.empty() && stopping) break;
            frame = std::move(frames.front());
            frames.pop_front();
            workerBusy = true;
        }
        std::string error;
        try {
            const auto png = Modules::EncodePng(frame.width, frame.height, frame.rgba);
            if (png.empty()) {
                error = "The captured frame could not be encoded.";
            } else {
                std::filesystem::create_directories(frame.file.parent_path());
                std::ofstream stream(frame.file, std::ios::binary | std::ios::trunc);
                stream.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
                if (!stream) error = "The screenshot file could not be written.";
            }
        } catch (const std::exception& exception) {
            error = exception.what();
        }
        Diagnostics::LogLine(error.empty() ? "info" : "warning", "screenshot",
            error.empty() ? std::format("saved {}", frame.file.string()) : error);
        SetResult(frame.file, std::move(error));
        {
            std::scoped_lock lock(mutex);
            workerBusy = false;
        }
    }
}

void EnsureWorkerLocked() {
    if (worker.joinable()) return;
    stopping = false;
    worker = std::jthread(RunWorker);
}

std::optional<PendingRequest> TakeRequest(bool includeHud) {
    std::scoped_lock lock(mutex);
    const auto found = std::ranges::find(requests, includeHud, &PendingRequest::includeHud);
    if (found == requests.end()) return std::nullopt;
    PendingRequest request = std::move(*found);
    requests.erase(found);
    return request;
}

void Requeue(PendingRequest request) {
    std::scoped_lock lock(mutex);
    requests.push_front(std::move(request));
}

}

bool Request(const std::filesystem::path& file, bool includeHud, std::string& error) {
    try {
        if (file.empty()) { error = "Choose a screenshot file."; return false; }
        std::scoped_lock lock(mutex);
        if (requests.size() >= MaxRequests) { error = "The screenshot queue is full."; return false; }
        EnsureWorkerLocked();
        requests.push_back({file, includeHud});
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

bool NeedsPresentWork() noexcept {
    try {
        std::scoped_lock lock(mutex);
        return !requests.empty() || gpuPending != 0 || !frames.empty() || workerBusy;
    } catch (...) { return false; }
}

Result CurrentResult() { std::scoped_lock lock(mutex); return result; }

std::filesystem::path ScreenshotDirectory() {
    PWSTR pictures{};
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, KF_FLAG_DEFAULT, nullptr, &pictures)) && pictures) {
        std::filesystem::path path(pictures);
        CoTaskMemFree(pictures);
        return path / L"Oinkster Screenshots";
    }
    if (pictures) CoTaskMemFree(pictures);
    return std::filesystem::temp_directory_path() / L"Oinkster Screenshots";
}

std::filesystem::path NextScreenshotFile() {
    const auto now = std::chrono::system_clock::now();
    const auto local = std::chrono::zoned_time{std::chrono::current_zone(), now}.get_local_time();
    const auto directory = ScreenshotDirectory();
    const auto stem = std::format(L"Oinkster {:%Y-%m-%d %H-%M-%S}", local);
    for (int suffix = 0; suffix < 1000; ++suffix) {
        const auto name = suffix == 0 ? stem + L".png" : std::format(L"{} ({}).png", stem, suffix);
        const auto file = directory / name;
        if (!std::filesystem::exists(file)) return file;
    }
    return directory / (stem + L".png");
}

void Shutdown() noexcept {
    try {
        {
            std::scoped_lock lock(mutex);
            requests.clear();
            stopping = true;
            wake.notify_all();
        }
        if (worker.joinable()) worker.join();
        std::scoped_lock lock(mutex);
        frames.clear();
        workerBusy = false;
    } catch (...) {}
}

void GpuCapture::Collect(UINT64 completedFence) {
    for (auto& slot : slots_) {
        if (!slot.pending || !slot.recorded || slot.fence == 0 || slot.fence > completedFence) continue;
        slot.pending = false;
        slot.recorded = false;
        {
            std::scoped_lock lock(mutex);
            if (gpuPending != 0) --gpuPending;
        }
        CpuFrame frame;
        frame.file = std::move(slot.file);
        frame.width = slot.width;
        frame.height = slot.height;
        frame.rgba.resize(static_cast<std::size_t>(frame.width) * frame.height * 4);
        const D3D12_RANGE read{0, static_cast<SIZE_T>(slot.footprint.Footprint.RowPitch) * frame.height};
        void* mapped{};
        if (FAILED(slot.buffer->Map(0, &read, &mapped)) || !mapped) {
            SetResult(frame.file, "The screenshot readback buffer could not be mapped.");
            continue;
        }
        for (UINT y = 0; y < frame.height; ++y) {
            const auto* source = static_cast<const std::uint8_t*>(mapped) + static_cast<std::size_t>(y) * slot.footprint.Footprint.RowPitch;
            auto* target = frame.rgba.data() + static_cast<std::size_t>(y) * frame.width * 4;
            for (UINT x = 0; x < frame.width; ++x) ConvertPixel(source + static_cast<std::size_t>(x) * 4, target + static_cast<std::size_t>(x) * 4, slot.format);
        }
        const D3D12_RANGE written{0, 0};
        slot.buffer->Unmap(0, &written);
        bool queued{};
        {
            std::scoped_lock lock(mutex);
            if (frames.size() < MaxFrames) {
                frames.push_back(std::move(frame));
                wake.notify_one();
                queued = true;
            }
        }
        if (!queued) SetResult(frame.file, "The screenshot encoder queue is full.");
    }
}

void GpuCapture::Record(ID3D12Device* device, ID3D12GraphicsCommandList* commandList,
    ID3D12Resource* backBuffer, bool includeHud) {
    if (!device || !commandList || !backBuffer) return;
    auto request = TakeRequest(includeHud);
    if (!request) return;
    const auto desc = backBuffer->GetDesc();
    if (!Supported(desc.Format)) {
        SetResult(request->file, "This display format cannot be captured; disable HDR and try again.");
        return;
    }
    const auto free = std::ranges::find_if(slots_, [](const Slot& slot) { return !slot.pending; });
    if (free == slots_.end()) { Requeue(std::move(*request)); return; }
    UINT64 total{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &total);
    if (!free->buffer || free->buffer->GetDesc().Width < total) {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = total;
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        free->buffer.Reset();
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr, IID_PPV_ARGS(&free->buffer)))) {
            SetResult(request->file, "Not enough video memory to capture a screenshot.");
            return;
        }
    }
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = backBuffer;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    commandList->ResourceBarrier(1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = free->buffer.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = backBuffer;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    commandList->ResourceBarrier(1, &barrier);
    free->footprint = footprint;
    free->file = std::move(request->file);
    free->width = static_cast<UINT>(desc.Width);
    free->height = desc.Height;
    free->format = desc.Format;
    free->fence = 0;
    free->pending = true;
    free->recorded = false;
    {
        std::scoped_lock lock(mutex);
        ++gpuPending;
    }
}

void GpuCapture::Submitted(UINT64 fenceValue) noexcept {
    for (auto& slot : slots_) {
        if (slot.pending && !slot.recorded) { slot.fence = fenceValue; slot.recorded = true; }
    }
}

void GpuCapture::Release() noexcept {
    std::size_t released{};
    for (auto& slot : slots_) {
        if (slot.pending) {
            ++released;
            SetResult(slot.file, "The screenshot was interrupted by a display reset.");
        }
        slot = Slot{};
    }
    if (released != 0) {
        std::scoped_lock lock(mutex);
        gpuPending = released > gpuPending ? 0 : gpuPending - released;
    }
}

}
