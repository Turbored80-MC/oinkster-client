#pragma once

#include "Oinkster/Module.hpp"
#include <d3d12.h>
#include <wrl/client.h>

// Cosmetic colour grading (the "post-processing" module) on Oinkster's own
// Present hook. The raw game frame is copied by FrameBlendResources; this
// pass draws that copy back over the backbuffer through a pixel shader that
// applies the module's grade. It uses ImGui's vertex layout and a root
// signature shaped like ImGui's (constants at b0, one SRV table at t0, a
// static sampler at s0), so it slots into the overlay draw list as a pair of
// callbacks around one full-screen image. No game memory is touched.
namespace Oinkster {

class PostProcessPass {
public:
    // Builds the root signature and pipeline for `format` (the overlay's
    // render-target format). False when the shader cannot be compiled.
    bool Ensure(ID3D12Device* device, DXGI_FORMAT format);
    void Release() noexcept;
    [[nodiscard]] bool Ready() const noexcept { return pipeline_ != nullptr; }

    // Binds the grading pipeline for the next draw. The caller resets the
    // overlay's own state afterwards (ImDrawCallback_ResetRenderState).
    void Bind(ID3D12GraphicsCommandList* commandList, float width, float height, const PostProcessSettings& settings) const;

private:
    Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_;
    DXGI_FORMAT format_{DXGI_FORMAT_UNKNOWN};
    bool failed_{};
};

// Constants the grading shader reads, packed in shader order (pure, tested).
struct PostProcessConstants {
    float projection[16]{};
    float grade[4]{};   // saturation, contrast, brightness, vignette
    float tint[4]{};    // r, g, b, strength
    float extra[4]{};   // sharpen, 1/width, 1/height, 0
};
[[nodiscard]] PostProcessConstants MakePostProcessConstants(float width, float height, const PostProcessSettings& settings) noexcept;

}
