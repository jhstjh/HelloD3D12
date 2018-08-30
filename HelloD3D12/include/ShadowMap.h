#pragma once

using namespace Microsoft::WRL;

namespace HDX
{

class ShadowMap
{
public:
    bool prepare(ID3D12Device* device,
        ID3D12CommandQueue*  commandQueue,
        ID3D12GraphicsCommandList* commandList,
        ID3D12DescriptorHeap* srvCBVHeap,
        UINT &heapOffset,
        ID3D12Resource* constantBuffer,
        UINT &constantBufferOffset,
        UINT8* cbDataBegin,
        UINT frameCount
    );

    void onRender(ID3D12GraphicsCommandList* cmdList);

    const ComPtr<ID3D12PipelineState> &getPipelineState() { return mPipelineState; }
    const ComPtr<ID3D12RootSignature> &getRootSignature() { return mRootSignature; }
    const ComPtr<ID3D12DescriptorHeap> &getDSVHeap() { return mDSVHeap; }
    const ComPtr<ID3D12Resource> &getDepthTexture() { return mDepthTexture; }
    const D3D12_CPU_DESCRIPTOR_HANDLE getSRVHandle() { return mSRVDescriptorStart; }

private:
    ComPtr<ID3D12DescriptorHeap> mDSVHeap;
    ComPtr<ID3D12Resource> mDepthTexture;
    D3D12_CPU_DESCRIPTOR_HANDLE mSRVDescriptorStart;

    ComPtr<ID3D12PipelineState> mPipelineState;
    ComPtr<ID3D12RootSignature> mRootSignature;
};

}