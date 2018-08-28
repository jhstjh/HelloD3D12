#pragma once
#include "d3d12Headers.h"
using namespace Microsoft::WRL;

namespace HDX
{

class SimpleShader
{
public:
    bool prepare(ID3D12Device* device);

    const ComPtr<ID3D12PipelineState> &getPipelineState() { return mPipelineState; }
    const ComPtr<ID3D12RootSignature> &getRootSignature() { return mRootSignature; }

private:
    ComPtr<ID3D12PipelineState> mPipelineState;
    ComPtr<ID3D12RootSignature> mRootSignature;
};


}