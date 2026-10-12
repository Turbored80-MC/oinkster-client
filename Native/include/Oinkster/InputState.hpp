#pragma once

#include <Windows.h>
#include <array>
#include <cstdint>
#include <vector>

namespace Oinkster {

class KeyStateTracker final {
public:
    // The menu and unload keys are the client's own controls, not gameplay
    // input: they must not be released into the game when the menu opens, nor
    // synthesized back when it closes. They are configurable, so they have to
    // be supplied rather than hardcoded - this used to test against a literal
    // 'K', which meant that after rebinding the menu key the NEW key was
    // treated as gameplay (so opening the menu pushed a WM_KEYUP for it into
    // the game, and closing pushed a WM_KEYDOWN back) while 'K' stayed
    // permanently excluded.
    void ConfigureControlKeys(std::uint32_t menuKey, std::uint32_t unloadKey) noexcept {
        menuKey_ = menuKey;
        unloadKey_ = unloadKey;
    }

    void Observe(std::uint32_t key, bool down) noexcept {
        if (key >= down_.size()) return;
        down_[key] = down;
        if (isolated_ && down && IsGameplayKey(key)) released_[key] = true;
    }

    [[nodiscard]] std::vector<std::uint32_t> BeginIsolation() {
        if (isolated_) return {};
        isolated_ = true;
        std::vector<std::uint32_t> keys;
        for (std::uint32_t key = 0; key < down_.size(); ++key) {
            if (!down_[key] || !IsGameplayKey(key)) continue;
            released_[key] = true;
            keys.push_back(key);
        }
        return keys;
    }

    [[nodiscard]] std::vector<std::uint32_t> EndIsolation(const std::array<bool, 256>& physicallyDown) {
        std::vector<std::uint32_t> keys;
        if (isolated_) {
            for (std::uint32_t key = 0; key < released_.size(); ++key) {
                if (released_[key] && physicallyDown[key] && IsGameplayKey(key)) keys.push_back(key);
            }
        }
        released_.fill(false);
        down_ = physicallyDown;
        isolated_ = false;
        return keys;
    }

    void Reset() noexcept {
        down_.fill(false);
        released_.fill(false);
        isolated_ = false;
    }

private:
    [[nodiscard]] bool IsGameplayKey(std::uint32_t key) const noexcept {
        return key < 256 && key != menuKey_ && key != unloadKey_
            && key != VK_ESCAPE && key != VK_LWIN && key != VK_RWIN;
    }

    std::array<bool, 256> down_{};
    std::array<bool, 256> released_{};
    bool isolated_{};
    // Defaults mirror Dx12Overlay's configuredMenuKey/configuredUnloadKey so a
    // tracker that is never configured behaves as before.
    std::uint32_t menuKey_{'K'};
    std::uint32_t unloadKey_{VK_OEM_5};
};

}
