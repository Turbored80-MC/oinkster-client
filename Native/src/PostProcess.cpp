#include "Oinkster/PostProcess.hpp"
#include "Oinkster/Diagnostics.hpp"
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

#pragma comment(lib, "d3dcompiler")

namespace Oinkster {
namespace {
using Microsoft::WRL::ComPtr;

constexpr UINT ConstantCount = sizeof(PostProcessConstants) / sizeof(float);

constexpr char VertexShader[] = R"(
cbuffer constants : register(b0) { float4x4 ProjectionMatrix; float4 grade; float4 tint; float4 extra; };
struct VS_INPUT { float2 pos : POSITION; float4 col : COLOR0; float2 uv : TEXCOORD0; };
struct PS_INPUT { float4 pos : SV_POSITION; float4 col : COLOR0; float2 uv : TEXCOORD0; };
PS_INPUT main(VS_INPUT input) {
    PS_INPUT output;
    output.pos = mul(ProjectionMatrix, float4(input.pos.xy, 0.f, 1.f));
    output.col = input.col;
    output.uv = input.uv;
    return output;
}
)";

constexpr char PixelShader[] = R"(
cbuffer constants : register(b0) { float4x4 ProjectionMatrix; float4 grade; float4 tint; float4 extra; };
struct PS_INPUT { float4 pos : SV_POSITION; float4 col : COLOR0; float2 uv : TEXCOORD0; };
SamplerState sampler0 : register(s0);
Texture2D texture0 : register(t0);
float4 main(PS_INPUT input) : SV_Target {
    float3 c = texture0.Sample(sampler0, input.uv).rgb;
    if (extra.x > 0.0) {
        float2 t = extra.yz;
        float3 n = texture0.Sample(sampler0, input.uv + float2(t.x, 0)).rgb
                 + texture0.Sample(sampler0, input.uv - float2(t.x, 0)).rgb
                 + texture0.Sample(sampler0, input.uv + float2(0, t.y)).rgb
                 + texture0.Sample(sampler0, input.uv - float2(0, t.y)).rgb;
        c += extra.x * (c * 4.0 - n) * 0.5;
    }
    c += grade.z;
    c = (c - 0.5) * grade.y + 0.5;
    float luma = dot(c, float3(0.2126, 0.7152, 0.0722));
    c = lerp(luma.xxx, c, grade.x);
    c = lerp(c, c * tint.rgb, tint.a);
    float2 d = input.uv - 0.5;
    c *= 1.0 - grade.w * smoothstep(0.15, 0.5, dot(d, d));
    return float4(saturate(c), input.col.a);
}
)";

bool Compile(const char* source, std::size_t length, const char* target, ComPtr<ID3DBlob>& blob) {
    ComPtr<ID3DBlob> errors;
    if (SUCCEEDED(D3DCompile(source, length, nullptr, nullptr, nullptr, "main", target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors))) {
        return true;
    }
    std::string message = errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()) : "unknown";
    Diagnostics::LogLine("warning", "post_process_shader", std::string(target) + ": " + message);
    return false;
}
}

PostProcessConstants MakePostProcessConstants(float width, float height, const PostProcessSettings& settings) noexcept {
    PostProcessConstants constants;
    // Same orthographic projection ImGui builds for a draw list at 0,0.
    const float left = 0.0F;
    const float right = std::max(width, 1.0F);
    const float top = 0.0F;
    const float bottom = std::max(height, 1.0F);
    const float projection[16]{
        2.0F / (right - left), 0.0F, 0.0F, 0.0F,
        0.0F, 2.0F / (top - bottom), 0.0F, 0.0F,
        0.0F, 0.0F, 0.5F, 0.0F,
        (right + left) / (left - right), (top + bottom) / (bottom - top), 0.5F, 1.0F};
    std::memcpy(constants.projection, projection, sizeof(projection));
    const auto finite = [](float value, float fallback) { return std::isfinite(value) ? value : fallback; };
    constants.grade[0] = std::clamp(finite(settings.saturation, 1.0F), 0.0F, 2.0F);
    constants.grade[1] = std::clamp(finite(settings.contrast, 1.0F), 0.5F, 1.5F);
    constants.grade[2] = std::clamp(finite(settings.brightness, 0.0F), -0.5F, 0.5F);
    constants.grade[3] = std::clamp(finite(settings.vignette, 0.0F), 0.0F, 1.0F);
    constants.tint[0] = static_cast<float>((settings.tint >> 16U) & 0xFFU) / 255.0F;
    constants.tint[1] = static_cast<float>((settings.tint >> 8U) & 0xFFU) / 255.0F;
    constants.tint[2] = static_cast<float>(settings.tint & 0xFFU) / 255.0F;
    constants.tint[3] = std::clamp(finite(settings.tintStrength, 0.0F), 0.0F, 1.0F);
    constants.extra[0] = std::clamp(finite(settings.sharpen, 0.0F), 0.0F, 1.0F);
    constants.extra[1] = 1.0F / right;
    constants.extra[2] = 1.0F / bottom;
    return constants;
}

bool PostProcessPass::Ensure(ID3D12Device* device, DXGI_FORMAT format) {
    if (Ready() && format == format_) return true;
    if (failed_ && format == format_) return false;
    Release();
    format_ = format;
    failed_ = true;
    if (!device || format == DXGI_FORMAT_UNKNOWN) return false;

    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 0;
    range.OffsetInDescriptorsFromTableStart = 0;
    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.ShaderRegister = 0;
    parameters[0].Constants.Num32BitValues = ConstantCount;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable.NumDescriptorRanges = 1;
    parameters[1].DescriptorTable.pDescriptorRanges = &range;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC description{};
    description.NumParameters = 2;
    description.pParameters = parameters;
    description.NumStaticSamplers = 1;
    description.pStaticSamplers = &sampler;
    description.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> serialized;
    if (FAILED(D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, nullptr))
        || FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&rootSignature_)))) {
        Diagnostics::LogLine("warning", "post_process_pipeline", "root signature creation failed");
        Release();
        return false;
    }

    ComPtr<ID3DBlob> vertex;
    ComPtr<ID3DBlob> pixel;
    if (!Compile(VertexShader, sizeof(VertexShader) - 1, "vs_5_0", vertex) || !Compile(PixelShader, sizeof(PixelShader) - 1, "ps_5_0", pixel)) {
        Release();
        return false;
    }
    // ImDrawVert: pos (8 bytes), uv (8 bytes), col (4 bytes).
    static const D3D12_INPUT_ELEMENT_DESC layout[]{
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{};
    pipeline.NodeMask = 1;
    pipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pipeline.pRootSignature = rootSignature_.Get();
    pipeline.SampleMask = UINT_MAX;
    pipeline.NumRenderTargets = 1;
    pipeline.RTVFormats[0] = format;
    pipeline.SampleDesc.Count = 1;
    pipeline.VS = {vertex->GetBufferPointer(), vertex->GetBufferSize()};
    pipeline.PS = {pixel->GetBufferPointer(), pixel->GetBufferSize()};
    pipeline.InputLayout = {layout, 3};
    auto& blend = pipeline.BlendState.RenderTarget[0];
    blend.BlendEnable = TRUE;
    blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
    blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    blend.BlendOp = D3D12_BLEND_OP_ADD;
    blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    auto& raster = pipeline.RasterizerState;
    raster.FillMode = D3D12_FILL_MODE_SOLID;
    raster.CullMode = D3D12_CULL_MODE_NONE;
    raster.DepthClipEnable = TRUE;
    pipeline.DepthStencilState.DepthEnable = FALSE;
    pipeline.DepthStencilState.StencilEnable = FALSE;
    if (FAILED(device->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&pipeline_)))) {
        Diagnostics::LogLine("warning", "post_process_pipeline", "pipeline creation failed");
        Release();
        return false;
    }
    failed_ = false;
    Diagnostics::LogLine("info", "post_process_pipeline", "colour grading pipeline ready");
    return true;
}

void PostProcessPass::Release() noexcept {
    pipeline_.Reset();
    rootSignature_.Reset();
}

void PostProcessPass::Bind(ID3D12GraphicsCommandList* commandList, float width, float height, const PostProcessSettings& settings) const {
    if (!commandList || !Ready()) return;
    const auto constants = MakePostProcessConstants(width, height, settings);
    commandList->SetPipelineState(pipeline_.Get());
    commandList->SetGraphicsRootSignature(rootSignature_.Get());
    commandList->SetGraphicsRoot32BitConstants(0, ConstantCount, &constants, 0);
}

}
