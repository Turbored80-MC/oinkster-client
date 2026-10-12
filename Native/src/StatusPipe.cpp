#include "Oinkster/StatusPipe.hpp"
#include "Oinkster/PipeWait.hpp"
#include <Windows.h>
#include <sddl.h>
#include <string_view>

namespace Oinkster {
namespace {
constexpr std::wstring_view PipeName = LR"(\\.\pipe\LOCAL\Oinkster.Client.Status.v1)";
}

StatusPipe::~StatusPipe() { Stop(); }

bool StatusPipe::Start(std::wstring initialStatus) {
    if (worker_.joinable()) return false;
    {
        std::scoped_lock lock(mutex_);
        status_ = std::move(initialStatus);
    }
    stopping_.store(false);
    // Manual-reset so every subsequent wait also sees the stop.
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stopEvent_) return false;
    worker_ = std::thread([this] { Run(); });
    return true;
}

void StatusPipe::Update(std::wstring status) {
    std::scoped_lock lock(mutex_);
    status_ = std::move(status);
}

void StatusPipe::Stop() noexcept {
    stopping_.store(true);
    if (stopEvent_) SetEvent(stopEvent_);
    if (worker_.joinable()) worker_.join();
    if (stopEvent_) {
        CloseHandle(stopEvent_);
        stopEvent_ = nullptr;
    }
}

void StatusPipe::Run() {
    PSECURITY_DESCRIPTOR descriptor{};
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"D:P(A;;GR;;;OW)(A;;GR;;;AC)(A;;GA;;;SY)", SDDL_REVISION_1, &descriptor, nullptr)) return;
    SECURITY_ATTRIBUTES attributes{ sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE };
    while (!stopping_.load()) {
        HANDLE pipe = CreateNamedPipeW(PipeName.data(),
            PIPE_ACCESS_OUTBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            1, 4096, 4096, 1000, &attributes);
        if (pipe == INVALID_HANDLE_VALUE) break;
        const auto connected = PipeWait::AwaitConnection(pipe, stopEvent_);
        if (connected == PipeWait::Result::Completed && !stopping_.load()) {
            std::wstring snapshot;
            { std::scoped_lock lock(mutex_); snapshot = status_; }
            // Bounded: a client that connects and never reads must not be able
            // to hold shutdown open.
            (void)PipeWait::Write(pipe, snapshot.data(),
                static_cast<DWORD>(snapshot.size() * sizeof(wchar_t)), stopEvent_, 2000);
            FlushFileBuffers(pipe);
            DisconnectNamedPipe(pipe);
        }
        CloseHandle(pipe);
        if (connected == PipeWait::Result::Stopped) break;
    }
    LocalFree(descriptor);
}

}
