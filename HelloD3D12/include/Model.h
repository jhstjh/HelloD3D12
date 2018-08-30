#pragma once

#include <string>
#include <vector>

using namespace Microsoft::WRL;
using namespace DirectX;

namespace HDX
{

class SimpleShader;
class ShadowMap;

class Model
{
public:
    struct Vertex
    {
        XMFLOAT3 pos;
        XMFLOAT2 uv;
        XMFLOAT3 normal;
    };

    struct SceneConstantBuffer
    {
        XMFLOAT4X4 worldViewProj;
    };

    struct SceneShadowConstantBuffer
    {
        XMFLOAT4X4 worldViewProj;
    };

    const UINT ConstantBufferSize = ((sizeof(SceneConstantBuffer) + 255) & ~255);
    const UINT ShadowConstantBufferSize = ((sizeof(SceneShadowConstantBuffer) + 255) & ~255);

    Model(std::string name, const XMFLOAT3& position);
    ~Model();

    bool prepare(ID3D12Device* device,
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
                 );
    void update(UINT frameIndex);

    void updateShadowDescriptors(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, ID3D12DescriptorHeap* currentFrameHeap, uint32_t &offset, UINT frameIndex);
    void updateDescriptors(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, ID3D12DescriptorHeap* currentFrameHeap, uint32_t &offset, UINT frameIndex);

    const ComPtr<ID3D12GraphicsCommandList> &getBundle() { return mBundle; }
    const ComPtr<ID3D12GraphicsCommandList> &getShadowBundle() { return mShadowBundle; }

private:
    static const UINT TextureWidth{ 256 };
    static const UINT TextureHeight{ 256 };
    static const UINT TexturePixelSize{ 4 };

    std::vector<Vertex> mVertices;
    std::vector<uint32_t> mIndices;

    XMMATRIX mViewMtx;
    XMMATRIX mShadowViewMtx;
    XMMATRIX mProjMtx;
    XMMATRIX mShadowProjMtx;

    XMFLOAT3 mPosition;

    std::string mFilename;

    ComPtr<ID3D12Resource> mVertexBuffer;
    D3D12_VERTEX_BUFFER_VIEW mVertexBufferView;
    ComPtr<ID3D12Resource> mIndexBuffer;
    D3D12_INDEX_BUFFER_VIEW mIndexBufferView;
    ComPtr<ID3D12Resource> mTexture;
    ComPtr<ID3D12CommandAllocator> mBundleAllocator;
    ComPtr<ID3D12GraphicsCommandList> mBundle;
    ComPtr<ID3D12GraphicsCommandList> mShadowBundle;
    SceneConstantBuffer mConstantBufferData{};
    SceneShadowConstantBuffer mShadowConstantBufferData{};
    UINT  mSRVCBVOffset{ 0 };
    UINT  mConstantBufferDataOffset{ 0 };
    UINT  mShadowConstantBufferDataOffset{ 0 };
    UINT8* mCBVDataBegin{ nullptr };

    ComPtr<ID3D12Resource> vertexBufferUploadHeap;
    ComPtr<ID3D12Resource> indexBufferUploadHeap;
    ComPtr<ID3D12Resource> textureUploadHeap; // scope!! Don't destroy it before finishing execute command queue.

    D3D12_CPU_DESCRIPTOR_HANDLE mSRVDescriptorStart;
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> mCBVDescriptorStart;
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> mShadowCBVDescriptorStart;

    ShadowMap* mShadowMap{ nullptr };
};

}