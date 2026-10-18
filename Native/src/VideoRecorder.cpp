#include "Oinkster/VideoRecorder.hpp"

#include "Oinkster/Diagnostics.hpp"
#include <Windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <format>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace Oinkster::VideoRecorder {
namespace {

using Microsoft::WRL::ComPtr;

// Frames waiting for the encoder; beyond this a new frame is dropped.
constexpr std::size_t MaxQueuedFrames = 6;
constexpr UINT32 DeclaredFrameRate = 60;

enum class PixelLayout { Rgba8, Bgra8, Rgb10A2 };

struct Frame {
    std::vector<std::uint8_t> pixels;  // tightly packed rows, 4 bytes per pixel
    UINT width{};
    UINT height{};
    PixelLayout layout{PixelLayout::Bgra8};
    Clock::time_point at{};
};

std::mutex mutex;
std::condition_variable wake;
std::deque<Frame> queue;
std::atomic_bool recording{};
bool stopRequested{};
State state;
std::jthread encoder;

std::optional<PixelLayout> LayoutOf(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return PixelLayout::Rgba8;
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM: case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return PixelLayout::Bgra8;
    case DXGI_FORMAT_R10G10B10A2_UNORM: return PixelLayout::Rgb10A2;
    default: return std::nullopt;
    }
}

void Fail(std::string error) {
    Diagnostics::LogLine("warning", "video_recorder", error);
    std::scoped_lock lock(mutex);
    if (state.error.empty()) state.error = std::move(error);
}

// One pixel row into Media Foundation's RGB32 (B, G, R, X in memory).
void ConvertRow(const std::uint8_t* source, std::uint8_t* target, UINT width, PixelLayout layout) {
    for (UINT x = 0; x < width; ++x, source += 4, target += 4) {
        switch (layout) {
        case PixelLayout::Bgra8:
            std::memcpy(target, source, 4);
            break;
        case PixelLayout::Rgba8:
            target[0] = source[2];
            target[1] = source[1];
            target[2] = source[0];
            target[3] = 255;
            break;
        case PixelLayout::Rgb10A2: {
            std::uint32_t value{};
            std::memcpy(&value, source, 4);
            target[0] = static_cast<std::uint8_t>((value >> 22U) & 0xFFU);
            target[1] = static_cast<std::uint8_t>((value >> 12U) & 0xFFU);
            target[2] = static_cast<std::uint8_t>((value >> 2U) & 0xFFU);
            target[3] = 255;
            break;
        }
        }
    }
}

class Writer {
public:
    // Opens the file for frames of width x height (cropped to even sizes,
    // which H.264 needs).
    HRESULT Open(const std::filesystem::path& file, UINT width, UINT height) {
        width_ = width & ~1U;
        height_ = height & ~1U;
        if (width_ < 16 || height_ < 16) return E_INVALIDARG;
        ComPtr<IMFAttributes> attributes;
        HRESULT result = MFCreateAttributes(&attributes, 1);
        if (SUCCEEDED(result)) result = attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        if (SUCCEEDED(result)) result = MFCreateSinkWriterFromURL(file.c_str(), nullptr, attributes.Get(), &writer_);
        const auto describe = [&](IMFMediaType* type, const GUID& subtype) {
            HRESULT hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_SUBTYPE, subtype);
            if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
            if (SUCCEEDED(hr)) hr = MFSetAttributeSize(type, MF_MT_FRAME_SIZE, width_, height_);
            if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(type, MF_MT_FRAME_RATE, DeclaredFrameRate, 1);
            if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
            return hr;
        };
        ComPtr<IMFMediaType> output;
        if (SUCCEEDED(result)) result = MFCreateMediaType(&output);
        if (SUCCEEDED(result)) result = describe(output.Get(), MFVideoFormat_H264);
        if (SUCCEEDED(result)) {
            const auto bitrate = std::clamp<std::uint64_t>(static_cast<std::uint64_t>(width_) * height_ * 6, 4'000'000, 40'000'000);
            result = output->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(bitrate));
        }
        if (SUCCEEDED(result)) result = writer_->AddStream(output.Get(), &stream_);
        ComPtr<IMFMediaType> input;
        if (SUCCEEDED(result)) result = MFCreateMediaType(&input);
        if (SUCCEEDED(result)) result = describe(input.Get(), MFVideoFormat_RGB32);
        // Positive stride: rows top-down, as the back buffer has them.
        if (SUCCEEDED(result)) result = input->SetUINT32(MF_MT_DEFAULT_STRIDE, width_ * 4);
        if (SUCCEEDED(result)) result = writer_->SetInputMediaType(stream_, input.Get(), nullptr);
        if (SUCCEEDED(result)) result = writer_->BeginWriting();
        return result;
    }

    HRESULT Write(const Frame& frame) {
        if (!started_) {
            first_ = frame.at;
            started_ = true;
        }
        const auto time = std::chrono::duration_cast<std::chrono::nanoseconds>(frame.at - first_).count() / 100;
        if (time <= last_ && last_ >= 0) return S_OK;  // never out of order
        last_ = time;
        const DWORD bytes = width_ * height_ * 4;
        ComPtr<IMFMediaBuffer> buffer;
        HRESULT result = MFCreateMemoryBuffer(bytes, &buffer);
        BYTE* data{};
        if (SUCCEEDED(result)) result = buffer->Lock(&data, nullptr, nullptr);
        if (SUCCEEDED(result)) {
            for (UINT y = 0; y < height_; ++y) {
                ConvertRow(frame.pixels.data() + static_cast<std::size_t>(y) * frame.width * 4, data + static_cast<std::size_t>(y) * width_ * 4,
                    width_, frame.layout);
            }
            (void)buffer->Unlock();
            result = buffer->SetCurrentLength(bytes);
        }
        ComPtr<IMFSample> sample;
        if (SUCCEEDED(result)) result = MFCreateSample(&sample);
        if (SUCCEEDED(result)) result = sample->AddBuffer(buffer.Get());
        if (SUCCEEDED(result)) result = sample->SetSampleTime(time);
        if (SUCCEEDED(result)) result = sample->SetSampleDuration(10'000'000 / DeclaredFrameRate);
        if (SUCCEEDED(result)) result = writer_->WriteSample(stream_, sample.Get());
        return result;
    }

    HRESULT Finish() { return writer_ ? writer_->Finalize() : S_OK; }
    [[nodiscard]] UINT Width() const noexcept { return width_; }
    [[nodiscard]] UINT Height() const noexcept { return height_; }

private:
    ComPtr<IMFSinkWriter> writer_;
    DWORD stream_{};
    UINT width_{};
    UINT height_{};
    bool started_{};
    Clock::time_point first_{};
    long long last_{-1};
};

void Encode(std::filesystem::path file) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const HRESULT startup = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    if (FAILED(startup)) {
        Fail(std::format("Media Foundation could not start (0x{:08X})", static_cast<std::uint32_t>(startup)));
    }
    std::optional<Writer> writer;
    bool broken = FAILED(startup);
    std::uint64_t written{};
    std::optional<Clock::time_point> firstAt;
    for (;;) {
        Frame frame;
        {
            std::unique_lock lock(mutex);
            wake.wait(lock, [] { return stopRequested || !queue.empty(); });
            if (queue.empty()) break;
            frame = std::move(queue.front());
            queue.pop_front();
        }
        if (broken) continue;
        if (!writer) {
            writer.emplace();
            if (const HRESULT opened = writer->Open(file, frame.width, frame.height); FAILED(opened)) {
                Fail(std::format("The video file could not be created (0x{:08X})", static_cast<std::uint32_t>(opened)));
                broken = true;
                writer.reset();
                continue;
            }
            Diagnostics::LogLine("info", "video_recorder", std::format("recording {}x{} to {}", writer->Width(), writer->Height(), file.string()));
        }
        if (frame.width < writer->Width() || frame.height < writer->Height()) {
            Fail("The game window changed size; the recording stopped there.");
            broken = true;
            continue;
        }
        if (const HRESULT wrote = writer->Write(frame); FAILED(wrote)) {
            Fail(std::format("Encoding a frame failed (0x{:08X})", static_cast<std::uint32_t>(wrote)));
            broken = true;
            continue;
        }
        ++written;
        if (!firstAt) firstAt = frame.at;
        std::scoped_lock lock(mutex);
        state.frames = written;
        state.seconds = std::chrono::duration<double>(frame.at - *firstAt).count();
    }
    if (writer && FAILED(writer->Finish())) Fail("The video could not be finished.");
    writer.reset();
    if (SUCCEEDED(startup)) (void)MFShutdown();
    if (SUCCEEDED(com)) CoUninitialize();
    std::scoped_lock lock(mutex);
    state.finishing = false;
    Diagnostics::LogLine("info", "video_recorder", std::format("finished: frames={} dropped={} error={}",
        state.frames, state.dropped, state.error.empty() ? "none" : state.error));
}

void Push(Frame frame) {
    std::scoped_lock lock(mutex);
    if (!recording.load(std::memory_order_acquire)) return;
    if (queue.size() >= MaxQueuedFrames) {
        ++state.dropped;
        return;
    }
    queue.push_back(std::move(frame));
    wake.notify_one();
}

}

bool WriteTestVideo(const std::filesystem::path& file, UINT width, UINT height, int count, std::string& error) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const HRESULT startup = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    HRESULT result = startup;
    {
        Writer writer;
        if (SUCCEEDED(result)) result = writer.Open(file, width, height);
        const auto start = Clock::now();
        for (int index = 0; index < count && SUCCEEDED(result); ++index) {
            Frame frame;
            frame.width = width;
            frame.height = height;
            frame.layout = PixelLayout::Rgba8;
            frame.at = start + std::chrono::milliseconds(16 * index);
            frame.pixels.resize(static_cast<std::size_t>(width) * height * 4);
            for (std::size_t pixel = 0; pixel < frame.pixels.size() / 4; ++pixel) {
                frame.pixels[pixel * 4] = static_cast<std::uint8_t>(pixel % width + static_cast<std::size_t>(index) * 4);
                frame.pixels[pixel * 4 + 1] = static_cast<std::uint8_t>(pixel / width);
                frame.pixels[pixel * 4 + 2] = 128;
            }
            result = writer.Write(frame);
        }
        if (SUCCEEDED(result)) result = writer.Finish();
    }
    if (SUCCEEDED(startup)) (void)MFShutdown();
    if (SUCCEEDED(com)) CoUninitialize();
    if (FAILED(result)) error = std::format("0x{:08X}", static_cast<std::uint32_t>(result));
    return SUCCEEDED(result);
}

bool Start(const std::filesystem::path& file, std::string& error) {
    try {
        std::scoped_lock lock(mutex);
        if (recording.load() || state.finishing) {
            error = "A video is already being recorded.";
            return false;
        }
        std::filesystem::create_directories(file.parent_path());
        if (encoder.joinable()) encoder.join();
        queue.clear();
        stopRequested = false;
        state = State{};
        state.recording = true;
        state.file = file;
        recording.store(true, std::memory_order_release);
        encoder = std::jthread([file] {
            try {
                Encode(file);
            } catch (...) {
                Fail("The video encoder stopped unexpectedly.");
                std::scoped_lock inner(mutex);
                state.finishing = false;
            }
        });
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        recording.store(false);
        return false;
    }
}

void Stop() noexcept {
    try {
        std::scoped_lock lock(mutex);
        if (!recording.exchange(false, std::memory_order_acq_rel)) return;
        state.recording = false;
        state.finishing = true;
        stopRequested = true;
        wake.notify_one();
    } catch (...) {}
}

bool Recording() noexcept { return recording.load(std::memory_order_acquire); }

State Current() {
    std::scoped_lock lock(mutex);
    return state;
}

void Shutdown() noexcept {
    Stop();
    try {
        if (encoder.joinable()) encoder.join();
    } catch (...) {}
}

void GpuCapture::Collect(UINT64 completedFence) {
    if (!ready_) return;
    const auto layout = LayoutOf(format_);
    for (auto& slot : slots_) {
        if (!slot.pending || !slot.recorded || slot.fence == 0 || slot.fence > completedFence) continue;
        slot.pending = false;
        slot.recorded = false;
        if (!layout || !recording.load(std::memory_order_acquire)) continue;
        Frame frame;
        frame.width = width_;
        frame.height = height_;
        frame.layout = *layout;
        frame.at = slot.at;
        frame.pixels.resize(static_cast<std::size_t>(width_) * height_ * 4);
        const D3D12_RANGE read{0, static_cast<SIZE_T>(footprint_.Footprint.RowPitch) * height_};
        void* mapped{};
        if (FAILED(slot.buffer->Map(0, &read, &mapped)) || !mapped) continue;
        for (UINT y = 0; y < height_; ++y) {
            std::memcpy(frame.pixels.data() + static_cast<std::size_t>(y) * width_ * 4,
                static_cast<const std::uint8_t*>(mapped) + static_cast<std::size_t>(y) * footprint_.Footprint.RowPitch,
                static_cast<std::size_t>(width_) * 4);
        }
        const D3D12_RANGE written{0, 0};
        slot.buffer->Unmap(0, &written);
        Push(std::move(frame));
    }
}

void GpuCapture::Record(ID3D12Device* device, ID3D12GraphicsCommandList* commandList, ID3D12Resource* backBuffer, Clock::time_point at) {
    if (!recording.load(std::memory_order_acquire) || !device || !commandList || !backBuffer) return;
    const auto desc = backBuffer->GetDesc();
    if (!LayoutOf(desc.Format)) {
        Fail(std::format("This display format ({}) cannot be recorded yet; turn off HDR in Minecraft's video settings.",
            static_cast<int>(desc.Format)));
        Stop();
        return;
    }
    if (!ready_ || desc.Width != width_ || desc.Height != height_ || desc.Format != format_) {
        Release();
        UINT64 total{};
        device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint_, nullptr, nullptr, &total);
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
        for (auto& slot : slots_) {
            if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
                    nullptr, IID_PPV_ARGS(&slot.buffer)))) {
                Release();
                Fail("Not enough video memory to record.");
                Stop();
                return;
            }
        }
        width_ = static_cast<UINT>(desc.Width);
        height_ = desc.Height;
        format_ = desc.Format;
        ready_ = true;
    }
    // A copy whose command list was never submitted is abandoned.
    for (auto& slot : slots_) {
        if (slot.pending && !slot.recorded) slot.pending = false;
    }
    const auto free = std::ranges::find_if(slots_, [](const Slot& slot) { return !slot.pending; });
    if (free == std::end(slots_)) {
        std::scoped_lock lock(mutex);
        ++state.dropped;
        return;
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
    destination.PlacedFootprint = footprint_;
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = backBuffer;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source.SubresourceIndex = 0;
    commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    commandList->ResourceBarrier(1, &barrier);
    free->pending = true;
    free->recorded = false;
    free->fence = 0;
    free->at = at;
}

void GpuCapture::Submitted(UINT64 fenceValue) noexcept {
    for (auto& slot : slots_) {
        if (slot.pending && !slot.recorded) {
            slot.fence = fenceValue;
            slot.recorded = true;
        }
    }
}

void GpuCapture::Release() noexcept {
    for (auto& slot : slots_) slot = Slot{};
    ready_ = false;
    width_ = 0;
    height_ = 0;
    format_ = DXGI_FORMAT_UNKNOWN;
}

}
