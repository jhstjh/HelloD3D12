#include "stdafx.h"

#include <vector>
#include <string>
#include <chrono>

#include "Asset.h"
#include "Model.h"
#include "SimpleShader.h"
#include "ShadowMap.h"

#define TINYOBJLOADER_IMPLEMENTATION
#include "ext/tiny_obj_loader.h"

#pragma warning( push )  
#pragma warning( disable : 4100 )  
#define STB_IMAGE_IMPLEMENTATION
#include "ext/stb_image.h"
#pragma warning( pop )   

template<typename CharT, typename TraitsT = std::char_traits<CharT> >
class vectorwrapbuf : public std::basic_streambuf<CharT, TraitsT> {
public:
    vectorwrapbuf(std::vector<CharT> &vec) {
        this->setg(vec.data(), vec.data(), vec.data() + vec.size());
    }
};

namespace HDX
{

Model::Model(std::string name, const XMFLOAT3& position)
    : mFilename(name)
    , mPosition(position)
{
}

Model::~Model()
{

}

bool Model::prepare(
    ID3D12Device* device,
    ID3D12CommandQueue*  commandQueue,
    ID3D12GraphicsCommandList* commandList,
    ID3D12DescriptorHeap* srvCBVHeap,
    UINT &heapOffset,
    SimpleShader* shader,
    ShadowMap* shadowMap,
    ID3D12Resource* constantBuffer,
    UINT &constantBufferOffset,
    UINT8* cbDataBegin,
    UINT frameCount
)
{
    mShadowMap = shadowMap;
    mSRVCBVOffset = heapOffset;
    mConstantBufferDataOffset = constantBufferOffset;
    mShadowConstantBufferDataOffset = mConstantBufferDataOffset + frameCount * (ConstantBufferSize + StaticConstantBufferSize);
    mCBVDataBegin = cbDataBegin;
    HR_ERROR_CHECK_CALL(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_BUNDLE, IID_PPV_ARGS(&mBundleAllocator)), false, "failed to create bundle allocator\n");

    {
        {
            tinyobj::attrib_t attrib;
            std::vector<tinyobj::shape_t> shapes;
            std::vector<tinyobj::material_t> materials;
            std::string err;

            std::string modelPath = "models/" + mFilename + ".obj";
            Asset obj(modelPath, 0);
            auto size = obj.getLength();
            std::vector<char> objData(size);
            obj.read(objData.data(), size);
            obj.close();

            vectorwrapbuf<char> databuf(objData);
            std::istream is(&databuf);

            if (!tinyobj::LoadObj(&attrib, &shapes, &materials, &err, &is))
            {
                assert(false);
            }

            for (const auto& shape : shapes)
            {
                for (const auto& index : shape.mesh.indices)
                {
                    Vertex vertex = {};

                    vertex.pos = {
                        attrib.vertices[3 * index.vertex_index + 0],
                        attrib.vertices[3 * index.vertex_index + 1],
                        attrib.vertices[3 * index.vertex_index + 2]
                    };

                    vertex.uv = {
                        attrib.texcoords[2 * index.texcoord_index + 0],
                        1.0f - attrib.texcoords[2 * index.texcoord_index + 1]
                    };

                    vertex.normal = {
                        attrib.normals[3 * index.normal_index + 0],
                        attrib.normals[3 * index.normal_index + 1],
                        attrib.normals[3 * index.normal_index + 2]
                    };

                    mVertices.push_back(vertex);
                    mIndices.push_back(static_cast<uint32_t>(mIndices.size()));
                }
            }
        }

        {
            const UINT vertexBufferSize = static_cast<UINT>(sizeof(Vertex) * mVertices.size());

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
            vertexData.pData = mVertices.data();
            vertexData.RowPitch = vertexBufferSize;
            vertexData.SlicePitch = vertexData.RowPitch;

            UpdateSubresources(commandList, mVertexBuffer.Get(), vertexBufferUploadHeap.Get(), 0, 0, 1, &vertexData);
            commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mVertexBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER));

            mVertexBufferView.BufferLocation = mVertexBuffer->GetGPUVirtualAddress();
            mVertexBufferView.StrideInBytes = sizeof(Vertex);
            mVertexBufferView.SizeInBytes = vertexBufferSize;
        }

        {
            const UINT indexBufferSize = static_cast<UINT>(sizeof(uint32_t) * mIndices.size());

            HR_ERROR_CHECK_CALL(device->CreateCommittedResource(
                &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
                D3D12_HEAP_FLAG_NONE,
                &CD3DX12_RESOURCE_DESC::Buffer(indexBufferSize),
                D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr,
                IID_PPV_ARGS(&mIndexBuffer)),
                false, "Failed to create committed resource\n");

            const UINT64 indexBufferUploadHeapSize = GetRequiredIntermediateSize(mIndexBuffer.Get(), 0, 1);

            HR_ERROR_CHECK_CALL(device->CreateCommittedResource(
                &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
                D3D12_HEAP_FLAG_NONE,
                &CD3DX12_RESOURCE_DESC::Buffer(indexBufferUploadHeapSize),
                D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr,
                IID_PPV_ARGS(&indexBufferUploadHeap)), false, "Failed to create index buffer upload heap\n");

            D3D12_SUBRESOURCE_DATA indexData{};
            indexData.pData = mIndices.data();
            indexData.RowPitch = indexBufferSize;
            indexData.SlicePitch = indexData.RowPitch;

            UpdateSubresources(commandList, mIndexBuffer.Get(), indexBufferUploadHeap.Get(), 0, 0, 1, &indexData);
            commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mIndexBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDEX_BUFFER));

            mIndexBufferView.BufferLocation = mIndexBuffer->GetGPUVirtualAddress();
            mIndexBufferView.SizeInBytes = indexBufferSize;
            mIndexBufferView.Format = DXGI_FORMAT_R32_UINT;
        }
    }


    CD3DX12_CPU_DESCRIPTOR_HANDLE srvCBVHandle(srvCBVHeap->GetCPUDescriptorHandleForHeapStart());
    UINT srvCBVDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    srvCBVHandle.Offset(1, mSRVCBVOffset);
    mSRVDescriptorStart = srvCBVHandle;
    heapOffset += srvCBVDescriptorSize;
    {
        std::string texturePath = "textures/" + mFilename + ".jpg";
        int32_t texWidth, texHeight, texChannels;
        Asset texFile(texturePath, 0);
        auto size = texFile.getLength();
        std::vector<uint8_t> texData(size);
        texFile.read(texData.data(), size);
        texFile.close();

        auto pixels = stbi_load_from_memory(texData.data(), size, &texWidth, &texHeight, &texChannels, STBI_rgb_alpha);
        assert(pixels);

        D3D12_RESOURCE_DESC textureDesc{};
        textureDesc.MipLevels = 1;
        textureDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        textureDesc.Width = texWidth;
        textureDesc.Height = texHeight;
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


        D3D12_SUBRESOURCE_DATA textureData{};
        textureData.pData = pixels;
        textureData.RowPitch = texWidth * TexturePixelSize;
        textureData.SlicePitch = textureData.RowPitch * texHeight;

        UpdateSubresources(commandList, mTexture.Get(), textureUploadHeap.Get(), 0, 0, 1, &textureData);
        commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mTexture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = textureDesc.Format;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(mTexture.Get(), &srvDesc, srvCBVHandle);

        stbi_image_free(pixels);
    }

    for (UINT i = 0; i < frameCount; i++)
    {
        {
            D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc{};
            cbvDesc.BufferLocation = constantBuffer->GetGPUVirtualAddress() + constantBufferOffset;
            cbvDesc.SizeInBytes = ConstantBufferSize;

            constantBufferOffset += cbvDesc.SizeInBytes;
            srvCBVHandle.Offset(1, srvCBVDescriptorSize);
            heapOffset += srvCBVDescriptorSize;

            device->CreateConstantBufferView(&cbvDesc, srvCBVHandle);
        }
        // only push this once. constant buffer and static constant buffer for one frame stays consecutively in the heap and will be bind together
        mCBVDescriptorStart.push_back(srvCBVHandle); 

        {
            D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc{};
            cbvDesc.BufferLocation = constantBuffer->GetGPUVirtualAddress() + constantBufferOffset;
            cbvDesc.SizeInBytes = StaticConstantBufferSize;

            constantBufferOffset += cbvDesc.SizeInBytes;
            srvCBVHandle.Offset(1, srvCBVDescriptorSize);
            heapOffset += srvCBVDescriptorSize;

            device->CreateConstantBufferView(&cbvDesc, srvCBVHandle);
        }
    }

    for (UINT i = 0; i < frameCount; i++)
    {
        D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc{};
        cbvDesc.BufferLocation = constantBuffer->GetGPUVirtualAddress() + constantBufferOffset;
        cbvDesc.SizeInBytes = ShadowConstantBufferSize;

        constantBufferOffset += cbvDesc.SizeInBytes;
        srvCBVHandle.Offset(1, srvCBVDescriptorSize);
        heapOffset += srvCBVDescriptorSize;

        device->CreateConstantBufferView(&cbvDesc, srvCBVHandle);

        mShadowCBVDescriptorStart.push_back(srvCBVHandle);
    }

    {
        auto pipelineState = shader->getPipelineState().Get();
        auto rootSignature = shader->getRootSignature().Get();
        HR_ERROR_CHECK_CALL(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_BUNDLE, mBundleAllocator.Get(), pipelineState, IID_PPV_ARGS(&mBundle)), false, "Failed to create bundle\n");
        mBundle->SetGraphicsRootSignature(rootSignature);
        mBundle->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        mBundle->IASetVertexBuffers(0, 1, &mVertexBufferView);
        mBundle->IASetIndexBuffer(&mIndexBufferView);
        mBundle->DrawInstanced(static_cast<UINT>(mIndices.size()), 1, 0, 0);
        HR_ERROR_CHECK_CALL(mBundle->Close(), false, "Failed to close bundle\n");
    }

    {
        auto pipelineState = shadowMap->getPipelineState().Get();
        auto rootSignature = shadowMap->getRootSignature().Get();
        HR_ERROR_CHECK_CALL(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_BUNDLE, mBundleAllocator.Get(), pipelineState, IID_PPV_ARGS(&mShadowBundle)), false, "Failed to create bundle\n");
        mShadowBundle->SetGraphicsRootSignature(rootSignature);
        mShadowBundle->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        mShadowBundle->IASetVertexBuffers(0, 1, &mVertexBufferView);
        mShadowBundle->IASetIndexBuffer(&mIndexBufferView);
        mShadowBundle->DrawInstanced(static_cast<UINT>(mIndices.size()), 1, 0, 0);
        HR_ERROR_CHECK_CALL(mShadowBundle->Close(), false, "Failed to close bundle\n");
    }

    mViewMtx = XMMatrixLookAtLH({ 4.0f, 4.0f, 4.0f }, { 0.f, 0.f, 0.f }, { 0.f, 1.f, 0.f });
    mShadowViewMtx = XMMatrixLookAtLH({ 2.f, 2.f, -2.f }, { 0.f, 0.f, 0.f }, { 0.f, 1.f, 0.f });
    mProjMtx = XMMatrixPerspectiveFovLH((45.0f) / 180.f * 3.1415926f, 16.f / 9.f, 0.1f, 10.f);
    mShadowProjMtx = XMMatrixOrthographicOffCenterLH(-5, 5, -5, 5, -5, 10);
    XMMATRIX ViewProjShadow = mShadowViewMtx * mShadowProjMtx;

    XMStoreFloat3(&mStaticConstantBufferData.lightDir, XMVector3Normalize({ -2.f, -2.f, 2.f }));
    XMStoreFloat4x4(&mStaticConstantBufferData.shadowViewProj, XMMatrixTranspose(ViewProjShadow));

    memcpy(mCBVDataBegin + mConstantBufferDataOffset + ConstantBufferSize, &mStaticConstantBufferData, sizeof(mStaticConstantBufferData));
    memcpy(mCBVDataBegin + mConstantBufferDataOffset + ConstantBufferSize + StaticConstantBufferSize + ConstantBufferSize, &mStaticConstantBufferData, sizeof(mStaticConstantBufferData));

    return true;
}

void Model::update(UINT frameIndex)
{
    static auto startTime = std::chrono::high_resolution_clock::now();

    auto currentTime = std::chrono::high_resolution_clock::now();
    float time = std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - startTime).count() / 1000.0f;

    XMMATRIX modelMtx = XMMatrixRotationY(time * 90.f / 180.f * 3.1415926f * ((mConstantBufferDataOffset == 0) ? 1.f : -1.f));
    modelMtx *= XMMatrixTranslation(mPosition.x, mPosition.y, mPosition.z);
    XMMATRIX modelViewProj = modelMtx * mViewMtx * mProjMtx;
    XMMATRIX modelViewProjShadow = modelMtx * mShadowViewMtx * mShadowProjMtx;

    XMStoreFloat4x4(&mConstantBufferData.worldViewProj, XMMatrixTranspose(modelViewProj));
    XMStoreFloat4x4(&mConstantBufferData.world, XMMatrixTranspose(modelMtx));
    XMStoreFloat4x4(&mShadowConstantBufferData.worldViewProj, XMMatrixTranspose(modelViewProjShadow));

    memcpy(mCBVDataBegin + mConstantBufferDataOffset + (ConstantBufferSize + StaticConstantBufferSize) * frameIndex, &mConstantBufferData, sizeof(mConstantBufferData));
    memcpy(mCBVDataBegin + mShadowConstantBufferDataOffset + ShadowConstantBufferSize * frameIndex, &mShadowConstantBufferData, sizeof(mShadowConstantBufferData));
}

void Model::updateShadowDescriptors(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, ID3D12DescriptorHeap* currentFrameHeap, uint32_t &offset, UINT frameIndex)
{
    UINT srvCBVDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    CD3DX12_CPU_DESCRIPTOR_HANDLE cpuHandle(currentFrameHeap->GetCPUDescriptorHandleForHeapStart());
    CD3DX12_GPU_DESCRIPTOR_HANDLE gpuHandle(currentFrameHeap->GetGPUDescriptorHandleForHeapStart());
    cpuHandle.Offset(offset, srvCBVDescriptorSize);
    gpuHandle.Offset(offset, srvCBVDescriptorSize);

    device->CopyDescriptorsSimple(1, cpuHandle, mShadowCBVDescriptorStart[frameIndex], D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    cmdList->SetGraphicsRootDescriptorTable(0, gpuHandle);
    gpuHandle.Offset(1, srvCBVDescriptorSize);
    offset++;
}

void Model::updateDescriptors(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, ID3D12DescriptorHeap* currentFrameHeap, uint32_t &offset, UINT frameIndex)
{
    UINT srvCBVDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    CD3DX12_CPU_DESCRIPTOR_HANDLE cpuHandle(currentFrameHeap->GetCPUDescriptorHandleForHeapStart());
    CD3DX12_GPU_DESCRIPTOR_HANDLE gpuHandle(currentFrameHeap->GetGPUDescriptorHandleForHeapStart());
    cpuHandle.Offset(offset, srvCBVDescriptorSize);
    gpuHandle.Offset(offset, srvCBVDescriptorSize);

    auto bindAndOffset = [&](uint32_t rootParamIndex, uint32_t handleCount)
    {
        cmdList->SetGraphicsRootDescriptorTable(rootParamIndex, gpuHandle);
        cpuHandle.Offset(handleCount, srvCBVDescriptorSize);
        gpuHandle.Offset(handleCount, srvCBVDescriptorSize);
        offset += handleCount;
    };

    device->CopyDescriptorsSimple(1, cpuHandle, mSRVDescriptorStart, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    bindAndOffset(0, 1);

    device->CopyDescriptorsSimple(2, cpuHandle, mCBVDescriptorStart[frameIndex], D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    bindAndOffset(1, 2);

    device->CopyDescriptorsSimple(1, cpuHandle, mShadowMap->getSRVHandle(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    bindAndOffset(2, 1);
}

}