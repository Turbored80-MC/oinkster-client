#pragma once

#include <d2d1_1.h>
#include <dwrite.h>
#include <wrl/client.h>

namespace Oinkster {

class Renderer final {
public:
    bool Attach(ID2D1DeviceContext* context) noexcept;
    void Resize(float width, float height, float dpi) noexcept;
    void RenderClickGui(bool visible) noexcept;
    void Detach() noexcept;
private:
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> panelBrush_;
    float width_{};
    float height_{};
    float dpi_{96.0F};
};

}

