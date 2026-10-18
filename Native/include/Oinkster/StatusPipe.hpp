#pragma once

#include <atomic>
#include <string>
#include <thread>
#include <mutex>

namespace Oinkster {

class StatusPipe final {
public:
    ~StatusPipe();
    bool Start(std::wstring initialStatus);
    void Update(std::wstring status);
    void Stop() noexcept;
private:
    void Run();
    std::atomic_bool stopping_{};
    // Signalled by Stop() to break the worker out of its overlapped
    // ConnectNamedPipe wait. Replaces the old "connect a throwaway client to
    // wake it" trick, which could miss and leave Stop() blocked in join()
    // forever (see Oinkster/PipeWait.hpp).
    void* stopEvent_{};
    std::thread worker_;
    std::mutex mutex_;
    std::wstring status_;
};

}
