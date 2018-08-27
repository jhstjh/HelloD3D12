#include <vector>

#include "d3d12Headers.h"
#include "Model.h"
#include "Helper.h"

namespace HDX
{

Model::Model()
{
}

Model::~Model()
{
    mConstantBuffer->Unmap(0, nullptr);
}

bool Model::prepare(
    ID3D12Device* device,
    ID3D12CommandQueue*  commandQueue,
    ID3D12GraphicsCommandList* commandList,
    ID3D12DescriptorHeap* srvCBVHeap,
    UINT heapOffset
)
{
    mSRVCBVOffset = heapOffset;
    HR_ERROR_CHECK_CALL(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_BUNDLE, IID_PPV_ARGS(&mBundleAllocator)), false, "failed to create bundle allocator\n");

    {
        D3D12_FEATURE_DATA_ROOT_SIGNATURE featureData{};

        featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;

        if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &featureData, sizeof(featureData))))
        {
            featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_0;
        }

        CD3DX12_DESCRIPTOR_RANGE1 ranges[2];
        ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC);
        ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC);

        CD3DX12_ROOT_PARAMETER1 rootParameters[2];
        rootParameters[0].InitAsDescriptorTable(1, &ranges[0], D3D12_SHADER_VISIBILITY_PIXEL);
        rootParameters[1].InitAsDescriptorTable(1, &ranges[1], D3D12_SHADER_VISIBILITY_VERTEX);

        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        sampler.MipLODBias = 0;
        sampler.MaxAnisotropy = 0;
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
        sampler.MinLOD = 0.f;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderRegister = 0;
        sampler.RegisterSpace = 0;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_FLAGS rootSignatureFlags =
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

        CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootSignatureDesc;
        rootSignatureDesc.Init_1_1(_countof(rootParameters), rootParameters, 1, &sampler, rootSignatureFlags);

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
            "   float4 offset;                                                 \n"
            "}                                                                 \n"
            "                                                                  \n"
            "struct PSInput                                                    \n"
            "{                                                                 \n"
            "	float4 position : SV_POSITION;                                 \n"
            "	float2 uv : TEXCOORD;                                          \n"
            "};                                                                \n"
            "                                                                  \n"
            "Texture2D g_texture : register(t0);                               \n"
            "SamplerState g_sampler : register(s0);                            \n"
            "                                                                  \n"
            "PSInput VSMain(float4 position : POSITION, float2 uv : TEXCOORD)  \n"
            "{                                                                 \n"
            "	PSInput result;                                                \n"
            "                                                                  \n"
            "	result.position = position + offset;                           \n"
            "	result.uv = uv;                                                \n"
            "                                                                  \n"
            "	return result;                                                 \n"
            "}                                                                 \n"
            "                                                                  \n"
            "float4 PSMain(PSInput input) : SV_TARGET                          \n"
            "{                                                                 \n"
            "	return g_texture.Sample(g_sampler, input.uv);                  \n"
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
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}
        };

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.InputLayout = { inputElementDescs, _countof(inputElementDescs) };
        psoDesc.pRootSignature = mRootSignature.Get();
        psoDesc.VS = CD3DX12_SHADER_BYTECODE(vertexShader.Get());
        psoDesc.PS = CD3DX12_SHADER_BYTECODE(pixelShader.Get());
        psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        psoDesc.DepthStencilState.DepthEnable = FALSE;
        psoDesc.DepthStencilState.StencilEnable = FALSE;
        psoDesc.SampleMask = UINT_MAX;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = 1;
        psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        psoDesc.SampleDesc.Count = 1;

        HR_ERROR_CHECK_CALL(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&mPipelineState)), false, "Failed to create graphics pipeline state\n");
    }

    {
        Vertex triangleVertices[] =
        {
            { { 0.0f, 0.25f * 16.f / 9.f, 0.0f }, { 0.5f, 0.0f } },
            { { 0.25f, -0.25f * 16.f / 9.f, 0.0f }, { 1.f, 1.f } },
            { { -0.25f, -0.25f * 16.f / 9.f, 0.0f }, { 0.f, 1.f } }
        };

        const UINT vertexBufferSize = sizeof(triangleVertices);

        HR_ERROR_CHECK_CALL(device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
            D3D12_HEAP_FLAG_NONE,
            &CD3DX12_RESOURCE_DESC::Buffer(vertexBufferSize),
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&mVertexBuffer)),
            false, "Failed to create committed resource\n");

        const UINT64 vertexBufferUploadHeapSize = GetRequiredIntermediateSize(mVertexBuffer.Get(), 0, 1);

        HR_ERROR_CHECK_CALL(device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
            D3D12_HEAP_FLAG_NONE,
            &CD3DX12_RESOURCE_DESC::Buffer(vertexBufferUploadHeapSize),
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&vertexBufferUploadHeap)), false, "Failed to create vertex buffer upload heap\n");

        D3D12_SUBRESOURCE_DATA vertexData{};
        vertexData.pData = triangleVertices;
        vertexData.RowPitch = vertexBufferSize;
        vertexData.SlicePitch = vertexData.RowPitch;

        UpdateSubresources(commandList, mVertexBuffer.Get(), vertexBufferUploadHeap.Get(), 0, 0, 1, &vertexData);
        commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mVertexBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER));

        mVertexBufferView.BufferLocation = mVertexBuffer->GetGPUVirtualAddress();
        mVertexBufferView.StrideInBytes = sizeof(Vertex);
        mVertexBufferView.SizeInBytes = vertexBufferSize;
    }


    CD3DX12_CPU_DESCRIPTOR_HANDLE srvCBVHandle(srvCBVHeap->GetCPUDescriptorHandleForHeapStart());
    UINT srvCBVDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    srvCBVHandle.Offset(1, mSRVCBVOffset);
    {
        D3D12_RESOURCE_DESC textureDesc{};
        textureDesc.MipLevels = 1;
        textureDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        textureDesc.Width = TextureWidth;
        textureDesc.Height = TextureHeight;
        textureDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
        textureDesc.DepthOrArraySize = 1;
        textureDesc.SampleDesc.Count = 1;
        textureDesc.SampleDesc.Quality = 0;
        textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;

        HR_ERROR_CHECK_CALL(device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
            D3D12_HEAP_FLAG_NONE,
            &textureDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&mTexture)), false, "Failed to create texture!\n");

        const UINT64 uploadBufferSize = GetRequiredIntermediateSize(mTexture.Get(), 0, 1);

        HR_ERROR_CHECK_CALL(device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
            D3D12_HEAP_FLAG_NONE,
            &CD3DX12_RESOURCE_DESC::Buffer(uploadBufferSize),
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&textureUploadHeap)), false, "Failed to create texture upload heap\n");


        auto generateTexDataFunc = [this](std::vector<UINT8> &data)
        {
            const UINT rowPitch = TextureWidth * TexturePixelSize;
            const UINT cellPitch = rowPitch >> 3;		// The width of a cell in the checkboard texture.
            const UINT cellHeight = TextureWidth >> 3;	// The height of a cell in the checkerboard texture.
            const UINT textureSize = rowPitch * TextureHeight;

            data.resize(textureSize);
            UINT8* pData = &data[0];

            for (UINT n = 0; n < textureSize; n += TexturePixelSize)
            {
                UINT x = n % rowPitch;
                UINT y = n / rowPitch;
                UINT i = x / cellPitch;
                UINT j = y / cellHeight;

                if (i % 2 == j % 2)
                {
                    pData[n] = 0x00;		// R
                    pData[n + 1] = 0x00;	// G
                    pData[n + 2] = 0x00;	// B
                    pData[n + 3] = 0xff;	// A
                }
                else
                {
                    pData[n] = 0xff;		// R
                    pData[n + 1] = 0xff;	// G
                    pData[n + 2] = 0xff;	// B
                    pData[n + 3] = 0xff;	// A
                }
            }

            return data;
        };

        std::vector<UINT8> texture;
        generateTexDataFunc(texture);

        D3D12_SUBRESOURCE_DATA textureData{};
        textureData.pData = texture.data();
        textureData.RowPitch = TextureWidth * TexturePixelSize;
        textureData.SlicePitch = textureData.RowPitch * TextureHeight;

        UpdateSubresources(commandList, mTexture.Get(), textureUploadHeap.Get(), 0, 0, 1, &textureData);
        commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mTexture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = textureDesc.Format;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(mTexture.Get(), &srvDesc, srvCBVHeap->GetCPUDescriptorHandleForHeapStart());
    }

    {
        HR_ERROR_CHECK_CALL(device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
            D3D12_HEAP_FLAG_NONE,
            &CD3DX12_RESOURCE_DESC::Buffer(1024 * 64),
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&mConstantBuffer)), false, "Failed to create constant buffer!\n");

        D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc{};
        cbvDesc.BufferLocation = mConstantBuffer->GetGPUVirtualAddress();
        cbvDesc.SizeInBytes = (sizeof(SceneConstantBuffer) + 255) & ~255;

        srvCBVHandle.Offset(1, srvCBVDescriptorSize);

        device->CreateConstantBufferView(&cbvDesc, srvCBVHandle);

        CD3DX12_RANGE readRange(0, 0);
        HR_ERROR_CHECK_CALL(mConstantBuffer->Map(0, &readRange, reinterpret_cast<void**>(&mCBVDataBegin)), false, "Faild to map constant buffer\n");
        memcpy(mCBVDataBegin, &mConstantBufferData, sizeof(mConstantBufferData));
    }

    HR_ERROR_CHECK_CALL(commandList->Close(), false, "Failed to close commandlist\n");
    ID3D12CommandList* ppCommandLists[] = { commandList };
    commandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);

    {
        HR_ERROR_CHECK_CALL(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_BUNDLE, mBundleAllocator.Get(), mPipelineState.Get(), IID_PPV_ARGS(&mBundle)), false, "Failed to create bundle\n");
        mBundle->SetGraphicsRootSignature(mRootSignature.Get());
        mBundle->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        mBundle->IASetVertexBuffers(0, 1, &mVertexBufferView);
        mBundle->DrawInstanced(3, 1, 0, 0);
        HR_ERROR_CHECK_CALL(mBundle->Close(), false, "Failed to close bundle\n");
    }

    return true;
}

void Model::update()
{
    const float translationSpeed = 0.005f;
    const float offsetBounds = 1.25f;

    mConstantBufferData.offset.x += translationSpeed;
    if (mConstantBufferData.offset.x > offsetBounds)
    {
        mConstantBufferData.offset.x = -offsetBounds;
    }

    memcpy(mCBVDataBegin, &mConstantBufferData, sizeof(mConstantBufferData));


}

}