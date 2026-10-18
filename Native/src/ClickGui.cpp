#include "Oinkster/ClickGui.hpp"
#include <algorithm>
#include <cwctype>

namespace Oinkster {
namespace {
constexpr std::string_view FavoritePrefix = "click-gui.favorite.";

std::wstring Lower(std::wstring value) {
    std::ranges::transform(value, value.begin(), [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return value;
}
}

void ClickGui::SetView(ModuleView view) noexcept {
    view_ = view;
    if (view != ModuleView::Category) category_.reset();
}

void ClickGui::SetCategory(ModuleCategory category) noexcept {
    category_ = category;
    view_ = ModuleView::Category;
}

std::vector<ModuleMetadata> ClickGui::FilteredCatalog() const {
    const auto query = Lower(search_);
    auto catalog = modules_.Catalog();
    std::erase_if(catalog, [&](const ModuleMetadata& item) {
        switch (view_) {
        case ModuleView::Favorites:
            if (!IsFavorite(item.id)) return true;
            break;
        case ModuleView::Enabled:
            if (!modules_.IsEnabled(item.id)) return true;
            break;
        case ModuleView::Available:
            if (!item.available) return true;
            break;
        case ModuleView::Roadmap:
            if (item.available) return true;
            break;
        case ModuleView::Category:
            if (!category_ || item.category != *category_) return true;
            break;
        case ModuleView::All:
            break;
        }
        if (query.empty()) return false;
        return !Lower(item.name).contains(query) && !Lower(item.description).contains(query);
    });
    std::ranges::sort(catalog, [](const ModuleMetadata& left, const ModuleMetadata& right) {
        if (left.name != right.name) return left.name < right.name;
        return left.id < right.id;
    });
    return catalog;
}

bool ClickGui::IsFavorite(std::string_view id) const {
    return modules_.ModuleSetting(std::string(FavoritePrefix) + std::string(id), "false") == "true";
}

bool ClickGui::ToggleFavorite(std::string_view id) {
    const auto catalog = modules_.Catalog();
    if (std::ranges::none_of(catalog, [&](const ModuleMetadata& item) { return item.id == id; })) return false;
    modules_.SetModuleSetting(std::string(FavoritePrefix) + std::string(id), IsFavorite(id) ? "false" : "true");
    return true;
}

bool ClickGui::ToggleModule(std::string_view id) noexcept {
    return modules_.SetEnabled(id, !modules_.IsEnabled(id));
}

std::optional<std::pair<std::string, std::uint32_t>> ClickGui::CaptureKey(std::uint32_t key) {
    if (!capturingModule_) return std::nullopt;
    auto result = std::pair{ *capturingModule_, key };
    capturingModule_.reset();
    return result;
}

void ClickGui::Reset() noexcept {
    modules_.DisableAll();
    search_.clear();
    view_ = ModuleView::All;
    category_.reset();
    capturingModule_.reset();
    hudEditMode_ = false;
}

}

