#include "stdafx.h"

#include "SimpleShader.h"
#include "Model.h"

namespace HDX
{

bool SimpleShader::prepare(ID3D12Device* device)
{
    {
        D3D12_FEATURE_DATA_ROOT_SIGNATURE featureData{};

        featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;

        if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &featureData, sizeof(featureData))))
        {
            featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_0;
        }

        CD3DX12_DESCRIPTOR_RANGE1 ranges[3];
        ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC);
        ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC);
        ranges[2].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1);

        CD3DX12_ROOT_PARAMETER1 rootParameters[3];
        rootParameters[0].InitAsDescriptorTable(1, &ranges[0], D3D12_SHADER_VISIBILITY_PIXEL);
        rootParameters[1].InitAsDescriptorTable(1, &ranges[1], D3D12_SHADER_VISIBILITY_ALL);
        rootParameters[2].InitAsDescriptorTable(1, &ranges[2], D3D12_SHADER_VISIBILITY_PIXEL);

        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sampler.MipLODBias = 0;
        sampler.MaxAnisotropy = 0;
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
        sampler.MinLOD = 0.f;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderRegister = 0;
        sampler.RegisterSpace = 0;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC shadowSampler{};
        shadowSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        shadowSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        shadowSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        shadowSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        shadowSampler.MipLODBias = 0;
        shadowSampler.MaxAnisotropy = 0;
        shadowSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        shadowSampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
        shadowSampler.MinLOD = 0.f;
        shadowSampler.MaxLOD = D3D12_FLOAT32_MAX;
        shadowSampler.ShaderRegister = 0;
        shadowSampler.RegisterSpace = 0;
        shadowSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_FLAGS rootSignatureFlags =
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

        D3D12_STATIC_SAMPLER_DESC samplers[] = { sampler };

        CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootSignatureDesc;
        rootSignatureDesc.Init_1_1(_countof(rootParameters), rootParameters, _countof(samplers), samplers, rootSignatureFlags);

        ComPtr<ID3DBlob> signature;
        ComPtr<ID3DBlob> error;

        HR_ERROR_CHECK_CALL(D3DX12SerializeVersionedRootSignature(&rootSignatureDesc, featureData.HighestVersion, &signature, &error), false, "Failed to serialize root signature\n");
        HR_ERROR_CHECK_CALL(device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&mRootSignature)), false, "Failed to create root signature\n");
    }

    {
        ComPtr<ID3DBlob> vertexShader;
        ComPtr<ID3DBlob> pixelShader;
        ComPtr<ID3DBlob> errorMsg;
#ifdef _DEBUG
        UINT compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
        UINT compileFlags = 0;
#endif
        const char* shader =
            "cbuffer SceneConstantBuffer : register(b0)\n                      \n"
            "{                                                                 \n"
            "   float4x4 gWorldViewProj;                                       \n"  
            "   float4x4 gShadowWorldViewProj;                                 \n"
            "}                                                                 \n"
            "                                                                  \n"
            "struct PSInput                                                    \n"
            "{                                                                 \n"
            "	float4 position : SV_POSITION;                                 \n"
            "	float2 uv : TEXCOORD;                                          \n"
            "   float3 normal : NORMAL;                                        \n"
            "   float4 shadowPosition : POSITION;                              \n"
            "};                                                                \n"
            "                                                                  \n"
            "Texture2D g_texture : register(t0);                               \n"
            "Texture2D g_shadowtexture : register(t1);                         \n"
            "SamplerState g_sampler : register(s0);                            \n"
            "                                                                  \n"
            "PSInput VSMain(float3 position : POSITION, float2 uv : TEXCOORD, float3 normal : NORMAL)  \n"
            "{                                                                 \n"
            "	PSInput result;                                                \n"
            "                                                                  \n"
            "	result.position = mul(float4(position, 1.0f), gWorldViewProj); \n"
            "	result.uv = uv;                                                \n"
            "   result.normal = normal;                                        \n"
            "   result.shadowPosition = mul(float4(position, 1.0f), gShadowWorldViewProj); \n                                                \n"
            "                                                                  \n"
            "	return result;                                                 \n"
            "}                                                                 \n"
            "                                                                  \n"
            "float4 PSMain(PSInput input) : SV_TARGET                          \n"
            "{                                                                 \n"
            "    float4 shadowPos = input.shadowPosition;                      \n"
            "    shadowPos.xyz /= shadowPos.w;                                 \n"
            "    float2 shadowCoord = 0.5f * shadowPos.xy + 0.5f;              \n"
            "    shadowCoord.y = 1.0f - shadowCoord.y;                         \n"
            "    float shadowDepth = shadowPos.z - 0.0005f;                    \n"
            "    float shadowMapDepth = g_shadowtexture.Sample(g_sampler, shadowCoord);  \n"
            "    float shadowScale = shadowMapDepth > shadowDepth ? 1.f : 0.2f;\n"
            "    return g_texture.Sample(g_sampler, input.uv) * shadowScale;   \n"
            "}                                                                 \n"
            ;

        if (FAILED(D3DCompile(shader, strlen(shader) + 1, nullptr, nullptr, nullptr, "VSMain", "vs_5_0", compileFlags, 0, &vertexShader, &errorMsg)))
        {
            if (errorMsg)
            {
                LOG_ERROR(reinterpret_cast<const char*>(errorMsg->GetBufferPointer()));
            }

            assert(false);
            return false;
        }

        if (FAILED(D3DCompile(shader, strlen(shader) + 1, nullptr, nullptr, nullptr, "PSMain", "ps_5_0", compileFlags, 0, &pixelShader, &errorMsg)))
        {
            if (errorMsg)
            {
                LOG_ERROR(reinterpret_cast<const char*>(errorMsg->GetBufferPointer()));
            }

            assert(false);
            return false;
        }

        D3D12_INPUT_ELEMENT_DESC inputElementDescs[]
        {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(Model::Vertex, uv), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Model::Vertex, normal), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}
        };

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.InputLayout = { inputElementDescs, _countof(inputElementDescs) };
        psoDesc.pRootSignature = mRootSignature.Get();
        psoDesc.VS = CD3DX12_SHADER_BYTECODE(vertexShader.Get());
        psoDesc.PS = CD3DX12_SHADER_BYTECODE(pixelShader.Get());
        psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        psoDesc.DepthStencilState.DepthEnable = TRUE;
        psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        psoDesc.DepthStencilState.StencilEnable = FALSE;
        psoDesc.SampleMask = UINT_MAX;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = 1;
        psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        psoDesc.SampleDesc.Count = 1;
        psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;

        HR_ERROR_CHECK_CALL(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&mPipelineState)), false, "Failed to create graphics pipeline state\n");
    }

    return true;
}

}