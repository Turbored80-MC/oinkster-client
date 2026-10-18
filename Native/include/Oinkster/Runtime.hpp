#pragma once

#include "BedrockAdapter.hpp"
#include "ModuleManager.hpp"
#include "SettingsStore.hpp"
#include "StatusPipe.hpp"
#include "ControlPipe.hpp"
#include <atomic>
#include <filesystem>
#include <memory>

namespace Oinkster {

class Runtime final {
public:
    explicit Runtime(std::filesystem::path dataDirectory);
    int Run() noexcept;
    void RequestStop() noexcept { stopping_.store(true); }
    [[nodiscard]] ModuleManager& Modules() noexcept { return modules_; }
private:
    std::filesystem::path dataDirectory_;
    ModuleManager modules_;
    std::unique_ptr<IBedrockAdapter> adapter_;
    StatusPipe statusPipe_;
    ControlPipe controlPipe_;
    std::atomic_bool stopping_{};
};

}
