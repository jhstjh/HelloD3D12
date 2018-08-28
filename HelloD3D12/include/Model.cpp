#include <vector>

#include "d3d12Headers.h"
#include "Model.h"
#include "Helper.h"
#include "SimpleShader.h"

namespace HDX
{

Model::Model()
{
}

Model::~Model()
{
    if (mConstantBuffer)
    {
        mConstantBuffer->Unmap(0, nullptr);
    }
}

bool Model::prepare(
    ID3D12Device* device,
    ID3D12CommandQueue*  commandQueue,
    ID3D12GraphicsCommandList* commandList,
    ID3D12DescriptorHeap* srvCBVHeap,
    UINT heapOffset,
    SimpleShader* shader
)
{
    mSRVCBVOffset = heapOffset;
    HR_ERROR_CHECK_CALL(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_BUNDLE, IID_PPV_ARGS(&mBundleAllocator)), false, "failed to create bundle allocator\n");

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
        auto pipelineState = shader->getPipelineState().Get();
        auto rootSignature = shader->getRootSignature().Get();
        HR_ERROR_CHECK_CALL(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_BUNDLE, mBundleAllocator.Get(), pipelineState, IID_PPV_ARGS(&mBundle)), false, "Failed to create bundle\n");
        mBundle->SetGraphicsRootSignature(rootSignature);
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