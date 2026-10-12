#pragma once

#include "ModuleManager.hpp"
#include <optional>

namespace Oinkster {

enum class ModuleView { All, Favorites, Enabled, Available, Roadmap, Category };

class ClickGui final {
public:
    explicit ClickGui(ModuleManager& modules) : modules_(modules) {}
    void ToggleVisible() noexcept { visible_ = !visible_; }
    [[nodiscard]] bool IsVisible() const noexcept { return visible_; }
    void SetSearch(std::wstring search) { search_ = std::move(search); }
    void SetView(ModuleView view) noexcept;
    [[nodiscard]] ModuleView View() const noexcept { return view_; }
    void SetCategory(ModuleCategory category) noexcept;
    [[nodiscard]] std::optional<ModuleCategory> Category() const noexcept { return category_; }
    [[nodiscard]] std::vector<ModuleMetadata> FilteredCatalog() const;
    [[nodiscard]] bool IsFavorite(std::string_view id) const;
    [[nodiscard]] bool ToggleFavorite(std::string_view id);
    [[nodiscard]] bool ToggleModule(std::string_view id) noexcept;
    void BeginKeyCapture(std::string moduleId) { capturingModule_ = std::move(moduleId); }
    [[nodiscard]] std::optional<std::pair<std::string, std::uint32_t>> CaptureKey(std::uint32_t key);
    void SetHudEditMode(bool enabled) noexcept { hudEditMode_ = enabled; }
    [[nodiscard]] bool IsHudEditMode() const noexcept { return hudEditMode_; }
    void Reset() noexcept;
private:
    ModuleManager& modules_;
    std::wstring search_;
    ModuleView view_{ModuleView::All};
    std::optional<ModuleCategory> category_;
    std::optional<std::string> capturingModule_;
    bool visible_{};
    bool hudEditMode_{};
};

}

