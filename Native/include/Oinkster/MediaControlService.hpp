#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace Oinkster {

struct MediaStatusSnapshot {
    bool brokerConnected{};
    bool available{};
    std::string sourceAppId;
    std::string title;
    std::string artist;
    std::string playbackStatus;
    bool canToggle{};
    bool canPrevious{};
    bool canNext{};
};

class IMediaControlService {
public:
    virtual ~IMediaControlService() = default;
    [[nodiscard]] virtual MediaStatusSnapshot Snapshot() const = 0;
    virtual void Queue(std::string_view command) = 0;
};

// The render and input threads only touch a cached snapshot/command queue.
// All named-pipe I/O happens on this worker.
class MediaControlService final : public IMediaControlService {
public:
    MediaControlService();
    ~MediaControlService() override;
    [[nodiscard]] MediaStatusSnapshot Snapshot() const override;
    void Queue(std::string_view command) override;

private:
    void Run() noexcept;
    void Exchange(std::string command) noexcept;

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    MediaStatusSnapshot snapshot_;
    std::string pendingCommand_;
    bool stopping_{};
    std::thread worker_;
};

[[nodiscard]] std::shared_ptr<IMediaControlService> MakeMediaControlService();

}
