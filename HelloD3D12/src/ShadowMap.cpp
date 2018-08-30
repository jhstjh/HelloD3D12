#include "stdafx.h"

#include <vector>

#include "ShadowMap.h"
#include "Model.h"

namespace HDX
{






bool ShadowMap::prepare(ID3D12Device * device, ID3D12CommandQueue * commandQueue, ID3D12GraphicsCommandList * commandList, ID3D12DescriptorHeap * srvCBVHeap, UINT & heapOffset, ID3D12Resource * constantBuffer, UINT & constantBufferOffset, UINT8 * cbDataBegin, UINT frameCount)
{
    // create depth texture
    {
        D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc{};
        dsvHeapDesc.NumDescriptors = frameCount;
        dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        HR_ERROR_CHECK_CALL(device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&mDSVHeap)), false, "Failed to create shadow DSV heap!\n");

        CD3DX12_CPU_DESCRIPTOR_HANDLE dsvHandle(mDSVHeap->GetCPUDescriptorHandleForHeapStart());
        {
            CD3DX12_RESOURCE_DESC depthTexDesc{
                D3D12_RESOURCE_DIMENSION_TEXTURE2D,
                0,
                2048,
                2048,
                1,
                1,
                DXGI_FORMAT_R32_TYPELESS,
                1,
                0,
                D3D12_TEXTURE_LAYOUT_UNKNOWN,
                D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL
            };

            D3D12_CLEAR_VALUE clearVal;
            clearVal.Format = DXGI_FORMAT_D32_FLOAT;
            clearVal.DepthStencil.Depth = 1.f;
            clearVal.DepthStencil.Stencil = 0;

            HR_ERROR_CHECK_CALL(device->CreateCommittedResource(
                &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
                D3D12_HEAP_FLAG_NONE,
                &depthTexDesc,
                D3D12_RESOURCE_STATE_DEPTH_WRITE,
                &clearVal,
                IID_PPV_ARGS(mDepthTexture.GetAddressOf())), false, "Failed to create shadow texture\n");
        }

        D3D12_DEPTH_STENCIL_VIEW_DESC depthStencilViewDesc{};
        depthStencilViewDesc.Format = DXGI_FORMAT_D32_FLOAT;
        depthStencilViewDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        depthStencilViewDesc.Texture2D.MipSlice = 0;
        device->CreateDepthStencilView(mDepthTexture.Get(), &depthStencilViewDesc, dsvHandle);

        CD3DX12_CPU_DESCRIPTOR_HANDLE srvCBVHandle(srvCBVHeap->GetCPUDescriptorHandleForHeapStart());
        UINT srvCBVDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        srvCBVHandle.Offset(1, heapOffset);
        mSRVDescriptorStart = srvCBVHandle;

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(mDepthTexture.Get(), &srvDesc, srvCBVHandle);

        heapOffset += srvCBVDescriptorSize;
    }

    // Create pipeline
    {
        D3D12_FEATURE_DATA_ROOT_SIGNATURE featureData{};

        featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;

        if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &featureData, sizeof(featureData))))
        {
            featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_0;
        }

        CD3DX12_DESCRIPTOR_RANGE1 ranges[1];
        ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC);

        CD3DX12_ROOT_PARAMETER1 rootParameters[1];
        rootParameters[0].InitAsDescriptorTable(1, &ranges[0], D3D12_SHADER_VISIBILITY_VERTEX);

        D3D12_ROOT_SIGNATURE_FLAGS rootSignatureFlags =
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_PIXEL_SHADER_ROOT_ACCESS;

        CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootSignatureDesc;
        rootSignatureDesc.Init_1_1(_countof(rootParameters), rootParameters, 0, nullptr, rootSignatureFlags);

        ComPtr<ID3DBlob> signature;
        ComPtr<ID3DBlob> error;

        HR_ERROR_CHECK_CALL(D3DX12SerializeVersionedRootSignature(&rootSignatureDesc, featureData.HighestVersion, &signature, &error), false, "Failed to serialize root signature\n");
        HR_ERROR_CHECK_CALL(device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&mRootSignature)), false, "Failed to create root signature\n");
    }

    {
        ComPtr<ID3DBlob> vertexShader;
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
            "}                                                                 \n"
            "                                                                  \n"
            "struct PSInput                                                    \n"
            "{                                                                 \n"
            "	float4 position : SV_POSITION;                                 \n"
            "	float2 uv : TEXCOORD;                                          \n"
            "   float3 normal : NORMAL;                                        \n"
            "};                                                                \n"
            "                                                                  \n"
            "Texture2D g_texture : register(t0);                               \n"
            "SamplerState g_sampler : register(s0);                            \n"
            "                                                                  \n"
            "PSInput VSMain(float3 position : POSITION, float2 uv : TEXCOORD, float3 normal : NORMAL)  \n"
            "{                                                                 \n"
            "	PSInput result;                                                \n"
            "                                                                  \n"
            "	result.position = mul(float4(position, 1.0f), gWorldViewProj); \n"
            "	result.uv = uv;                                                \n"
            "   result.normal = normal;                                        \n"
            "                                                                  \n"
            "	return result;                                                 \n"
            "}                                                                 \n";

        if (FAILED(D3DCompile(shader, strlen(shader) + 1, nullptr, nullptr, nullptr, "VSMain", "vs_5_0", compileFlags, 0, &vertexShader, &errorMsg)))
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

void ShadowMap::onRender(ID3D12GraphicsCommandList* cmdList)
{
    cmdList->SetGraphicsRootSignature(mRootSignature.Get());



}

}