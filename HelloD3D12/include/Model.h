#pragma once

#include <string>
#include <vector>

using namespace Microsoft::WRL;
using namespace DirectX;

namespace HDX
{

class SimpleShader;

class Model
{
public:
    struct Vertex
    {
        XMFLOAT3 pos;
        XMFLOAT2 uv;
        XMFLOAT3 normal;
    };

    Model(std::string name);
    ~Model();

    bool prepare(ID3D12Device* device,
                 ID3D12CommandQueue*  commandQueue,
                 ID3D12GraphicsCommandList* commandList,
                 ID3D12DescriptorHeap* srvCBVHeap,
                 UINT heapOffset,
                 SimpleShader* shader
                 );
    void update();

    const ComPtr<ID3D12GraphicsCommandList> &getBundle() { return mBundle; }

private:
    struct SceneConstantBuffer
    {
        XMFLOAT4X4 worldViewProj;
    };

    static const UINT TextureWidth{ 256 };
    static const UINT TextureHeight{ 256 };
    static const UINT TexturePixelSize{ 4 };

    std::vector<Vertex> mVertices;
    std::vector<uint32_t> mIndices;

    XMMATRIX mViewMtx;
    XMMATRIX mProjMtx;

    std::string mFilename;

    ComPtr<ID3D12Resource> mVertexBuffer;
    D3D12_VERTEX_BUFFER_VIEW mVertexBufferView;
    ComPtr<ID3D12Resource> mIndexBuffer;
    D3D12_INDEX_BUFFER_VIEW mIndexBufferView;
    ComPtr<ID3D12Resource> mTexture;
    ComPtr<ID3D12CommandAllocator> mBundleAllocator;
    ComPtr<ID3D12GraphicsCommandList> mBundle;
    ComPtr<ID3D12Resource> mConstantBuffer;
    UINT8* mCBVDataBegin{ nullptr };
    SceneConstantBuffer mConstantBufferData{};
    UINT  mSRVCBVOffset{ 0 };


    ComPtr<ID3D12Resource> vertexBufferUploadHeap;
    ComPtr<ID3D12Resource> indexBufferUploadHeap;
    ComPtr<ID3D12Resource> textureUploadHeap; // scope!! Don't destroy it before finishing execute command queue.
};

}