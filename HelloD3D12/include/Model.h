#pragma once

using namespace Microsoft::WRL;
using namespace DirectX;

namespace HDX
{

class SimpleShader;

class Model
{
public:
    Model();
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
    struct Vertex
    {
        XMFLOAT3 position;
        XMFLOAT2 uv;
    };

    struct SceneConstantBuffer
    {
        XMFLOAT4 offset;
    };

    static const UINT TextureWidth{ 256 };
    static const UINT TextureHeight{ 256 };
    static const UINT TexturePixelSize{ 4 };

    ComPtr<ID3D12Resource> mVertexBuffer;
    D3D12_VERTEX_BUFFER_VIEW mVertexBufferView;
    ComPtr<ID3D12Resource> mTexture;
    ComPtr<ID3D12CommandAllocator> mBundleAllocator;
    ComPtr<ID3D12GraphicsCommandList> mBundle;
    ComPtr<ID3D12Resource> mConstantBuffer;
    UINT8* mCBVDataBegin{ nullptr };
    SceneConstantBuffer mConstantBufferData{};
    UINT  mSRVCBVOffset{ 0 };


    ComPtr<ID3D12Resource> vertexBufferUploadHeap;
    ComPtr<ID3D12Resource> textureUploadHeap; // scope!! Don't destroy it before finishing execute command queue.
};

}