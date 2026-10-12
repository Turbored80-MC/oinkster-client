#pragma once

#include <atomic>
#include <thread>
#include <filesystem>

namespace Oinkster {

class ControlPipe final {
public:
    explicit ControlPipe(std::filesystem::path settingsPath) : settingsPath_(std::move(settingsPath)) {}
    ~ControlPipe();
    bool Start();
    [[nodiscard]] bool ConsumeReloadRequested() noexcept { return reloadRequested_.exchange(false); }
    void Stop() noexcept;
private:
    void Run() noexcept;
    std::atomic_bool stopping_{};
    std::atomic_bool reloadRequested_{};
    // See StatusPipe::stopEvent_ and Oinkster/PipeWait.hpp - breaks the worker
    // out of its overlapped accept so Stop() can never block indefinitely.
    void* stopEvent_{};
    std::thread worker_;
    std::filesystem::path settingsPath_;
};

}
