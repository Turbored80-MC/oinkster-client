#include "Oinkster/MediaControlService.hpp"
#include "Oinkster/Json.hpp"
#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <format>
#include <utility>

namespace Oinkster {
namespace {
constexpr std::wstring_view PipeName = LR"(\\.\pipe\LOCAL\Oinkster.Media.v1)";
constexpr DWORD PipeTimeoutMilliseconds = 750;
constexpr std::size_t MaximumReplyBytes = 16 * 1024;

std::string StringMember(const Json::Value& root, std::string_view name) {
    const auto* value = root.Find(name);
    const auto* text = value ? value->AsString() : nullptr;
    return text ? *text : std::string{};
}

bool BoolMember(const Json::Value& root, std::string_view name) {
    const auto* value = root.Find(name);
    const auto* boolean = value ? value->AsBool() : nullptr;
    return boolean && *boolean;
}
}

MediaControlService::MediaControlService() : worker_([this] { Run(); }) {}

MediaControlService::~MediaControlService() {
    {
        std::scoped_lock lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
}

MediaStatusSnapshot MediaControlService::Snapshot() const {
    std::scoped_lock lock(mutex_);
    return snapshot_;
}

void MediaControlService::Queue(std::string_view command) {
    if (command != "previous" && command != "toggle-play-pause" && command != "next") return;
    {
        std::scoped_lock lock(mutex_);
        pendingCommand_ = command;
    }
    wake_.notify_one();
}

void MediaControlService::Run() noexcept {
    while (true) {
        std::string command = "status";
        {
            std::unique_lock lock(mutex_);
            wake_.wait_for(lock, std::chrono::milliseconds(750), [this] { return stopping_ || !pendingCommand_.empty(); });
            if (stopping_) return;
            if (!pendingCommand_.empty()) command = std::exchange(pendingCommand_, {});
        }
        Exchange(std::move(command));
    }
}

void MediaControlService::Exchange(std::string command) noexcept {
    try {
        const std::string request = std::format(R"({{"protocolVersion":1,"command":"{}"}}\n)", command);
        std::string reply(MaximumReplyBytes, '\0');
        DWORD read{};
        const BOOL ok = CallNamedPipeW(PipeName.data(),
            const_cast<char*>(request.data()), static_cast<DWORD>(request.size()),
            reply.data(), static_cast<DWORD>(reply.size()), &read, PipeTimeoutMilliseconds);
        if (!ok || read == 0 || read >= reply.size()) {
            std::scoped_lock lock(mutex_);
            snapshot_ = {};
            return;
        }
        reply.resize(read);
        const auto parsed = Json::Parse(reply);
        const auto* version = parsed ? parsed->Find("protocolVersion") : nullptr;
        const auto* number = version ? version->AsNumber() : nullptr;
        if (!parsed || !parsed->AsObject() || !number || *number != 1.0) {
            std::scoped_lock lock(mutex_);
            snapshot_ = {};
            return;
        }
        MediaStatusSnapshot next;
        next.brokerConnected = true;
        next.available = BoolMember(*parsed, "available");
        next.sourceAppId = StringMember(*parsed, "sourceAppId");
        next.title = StringMember(*parsed, "title");
        next.artist = StringMember(*parsed, "artist");
        next.playbackStatus = StringMember(*parsed, "playbackStatus");
        next.canToggle = BoolMember(*parsed, "canToggle");
        next.canPrevious = BoolMember(*parsed, "canPrevious");
        next.canNext = BoolMember(*parsed, "canNext");
        std::scoped_lock lock(mutex_);
        snapshot_ = std::move(next);
    } catch (...) {
        std::scoped_lock lock(mutex_);
        snapshot_ = {};
    }
}

std::shared_ptr<IMediaControlService> MakeMediaControlService() {
    return std::make_shared<MediaControlService>();
}

}
