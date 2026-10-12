#include "Oinkster/Renderer.hpp"
#include <d2d1helper.h>

namespace Oinkster {

bool Renderer::Attach(ID2D1DeviceContext* context) noexcept {
    Detach();
    if (!context) return false;
    context_ = context;
    const auto color = D2D1::ColorF(0.075F, 0.067F, 0.082F, 0.96F);
    return SUCCEEDED(context_->CreateSolidColorBrush(color, &panelBrush_));
}

void Renderer::Resize(float width, float height, float dpi) noexcept { width_ = width; height_ = height; dpi_ = dpi; }

void Renderer::RenderClickGui(bool visible) noexcept {
    if (!visible || !context_ || !panelBrush_) return;
    const float scale = dpi_ / 96.0F;
    const auto panelWidth = (width_ < 900.0F ? width_ - 40.0F : 820.0F) / scale;
    const auto panelHeight = (height_ < 640.0F ? height_ - 40.0F : 560.0F) / scale;
    context_->FillRoundedRectangle(D2D1::RoundedRect(
        D2D1::RectF(20.0F, 20.0F, 20.0F + panelWidth, 20.0F + panelHeight), 16.0F, 16.0F), panelBrush_.Get());
}

void Renderer::Detach() noexcept { panelBrush_.Reset(); context_.Reset(); }

}
