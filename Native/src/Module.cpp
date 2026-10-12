#include "Oinkster/Module.hpp"
#include <algorithm>
#include <cmath>

namespace Oinkster {

HudLayout ClampHudLayout(HudLayout value) noexcept {
    const auto finiteOr = [](float candidate, float fallback) { return std::isfinite(candidate) ? candidate : fallback; };
    value.normalizedX = std::clamp(finiteOr(value.normalizedX, 0.02F), 0.0F, 1.0F);
    value.normalizedY = std::clamp(finiteOr(value.normalizedY, 0.05F), 0.0F, 1.0F);
    value.scale = std::clamp(finiteOr(value.scale, 1.0F), 0.65F, 2.0F);
    value.backgroundOpacity = std::clamp(finiteOr(value.backgroundOpacity, 0.88F), 0.0F, 1.0F);
    return value;
}

std::string IHudModule::DisplayText() const {
    const auto content = Content();
    std::string text;
    const auto append = [&](const std::string& line) {
        if (!text.empty()) text += '\n';
        text += line;
    };
    for (const auto& line : content.lines) append(line.text);
    for (const auto& row : content.keyRows) {
        std::string line;
        for (const auto& key : row) {
            if (key.spacer) continue;
            if (!line.empty()) line += ' ';
            line += key.pressed ? "[" + key.label + "]" : " " + key.label + " ";
            if (!key.subLabel.empty()) line += "(" + key.subLabel + ")";
        }
        append(line);
    }
    return text;
}

}
