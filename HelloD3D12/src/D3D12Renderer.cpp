#include "stdafx.h"

#include <assert.h>
#include <memory>
#include <vector>
#include "Renderer.h"

#include "Model.h"
#include "SimpleShader.h"
#include "ShadowMap.h"

using namespace Microsoft::WRL;
using namespace DirectX;

namespace HDX
{

static Renderer* gInstance{ nullptr };

class D3D12RendererImpl : public Renderer
{
public:
    void release() final
    {
        if (mIsInitialized)
        {
            waitForGPU();
            CloseHandle(mFenceEvent);
            mIsInitialized = false;
        }

        assert(gInstance == this);
        gInstance = nullptr;
        delete this;
    }

    bool isInitialized() const final
    {
        return mIsInitialized;
    }

    bool init(HWND hWnd, uint32_t width, uint32_t height) final
    {
        mWidth = width;
        mHeight = height;
        mAspectRatio = static_cast<float>(mWidth) / static_cast<float>(mHeight);

        mViewport = CD3DX12_VIEWPORT(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
        mScissorRect = CD3DX12_RECT(0, 0, static_cast<LONG>(width), static_cast<LONG>(height));

        mShadowViewport = CD3DX12_VIEWPORT(0.0f, 0.0f, 2048.f, 2048.f);
        mShadowScissorRect = CD3DX12_RECT(0, 0, 2048, 2048);

        auto modelChalet = std::make_unique<Model>(
#ifndef _DEBUG
        "chalet"
#else      
        "cube"
#endif
            , XMFLOAT3{ 0.f, 0.f, 0.f });
        mModels.push_back(std::move(modelChalet));

        auto modelCube = std::make_unique<Model>("cube", XMFLOAT3{ 0.f, 0.f, 2.f });
        mModels.push_back(std::move(modelCube));

        mSimpleShader = std::make_unique<SimpleShader>();
        mShadowMap = std::make_unique<ShadowMap>();

        if (!loadPipeline(hWnd))
        {
            return false;
        }

        if (!loadAssets())
        {
            return false;
        }

        mIsInitialized = true;
        return true;
    }

    void onRender() final
    {
        if (!mIsInitialized)
        {
            return;
        }

        for (auto const& model : mModels)
        {
            model->update(mFrameIndex);
        }

        HR_ERROR_CHECK_CALL(mCommandAllocator[mFrameIndex]->Reset(), void(), "Failed to reset command allocator\n");

        uint32_t frameHeapOffset = 0;
        populateShadowCommandList(frameHeapOffset);
        ID3D12CommandList* ppCommadLists[] = { mCommandList.Get() };
        mCommandQueue->ExecuteCommandLists(_countof(ppCommadLists), ppCommadLists);

        insertGPUFence();

        populateCommandList(frameHeapOffset);
        mCommandQueue->ExecuteCommandLists(_countof(ppCommadLists), ppCommadLists);

        if (FAILED(mSwapChain->Present(1, 0)))
        {
            assert(false);
        }

        MoveToNextFrame();
    }

private:

    bool loadPipeline(HWND hWnd)
    {
        UINT dxgiFactoryFlag{ 0 };

#ifdef _DEBUG
#pragma warning( push )  
#pragma warning(disable:4996)
        ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
        {
            if (!(getenv("NVTX_INJECTION64_PATH") || getenv("NSIGHT_LAUNCHED")))
            {
                debugController->EnableDebugLayer();
                dxgiFactoryFlag |= DXGI_CREATE_FACTORY_DEBUG;
            }
        }
#pragma warning( pop )  
#endif

        ComPtr<IDXGIFactory4> factory;
        HR_ERROR_CHECK_CALL(CreateDXGIFactory2(dxgiFactoryFlag, IID_PPV_ARGS(&factory)), false, "Failed to create DXGI factory!\n");

        ComPtr<IDXGIAdapter1> adapter = nullptr;
        for (UINT adapterIndex = 0; DXGI_ERROR_NOT_FOUND != factory->EnumAdapters1(adapterIndex, &adapter); ++adapterIndex)
        {
            DXGI_ADAPTER_DESC1 desc;
            adapter->GetDesc1(&desc);

            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
            {
                continue;
            }

            if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_1, __uuidof(ID3D12Device), nullptr)))
            {
                break;
            }
        }

        HR_ERROR_CHECK_CALL(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&mDevice)), false, "Failed to create D3D12 Device!\n");

#ifdef _DEBUG
#pragma warning( push )  
#pragma warning(disable:4996)
        ComPtr<ID3D12InfoQueue> infoQueue;
        if (SUCCEEDED(mDevice.As(&infoQueue)))
        {
            if (!(getenv("NVTX_INJECTION64_PATH") || getenv("NSIGHT_LAUNCHED")))
            {
                infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
                infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
                infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, TRUE);
            }
            // .. add filter
        }
#pragma warning( pop )  
#endif

        D3D12_COMMAND_QUEUE_DESC queueDesc {};
        queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

        HR_ERROR_CHECK_CALL(mDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&mCommandQueue)), false, "Failed to create command queue!\n");

        DXGI_SWAP_CHAIN_DESC1 swapChainDesc{};
        swapChainDesc.BufferCount = FrameCount;
        swapChainDesc.Width = mWidth;
        swapChainDesc.Height = mHeight;
        swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        swapChainDesc.SampleDesc.Count = 1;

        ComPtr<IDXGISwapChain1> swapChain;
        HR_ERROR_CHECK_CALL(factory->CreateSwapChainForHwnd(mCommandQueue.Get(), hWnd, &swapChainDesc, nullptr, nullptr, &swapChain), false, "Failed to create swap chain!\n");
        HR_ERROR_CHECK_CALL(factory->MakeWindowAssociation(hWnd, DXGI_MWA_NO_ALT_ENTER), false, "Failed to change windows association!\n");
        HR_ERROR_CHECK_CALL(swapChain.As(&mSwapChain), false, "Failed to cast swap chain!\n");

        mFrameIndex = mSwapChain->GetCurrentBackBufferIndex();

        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
        rtvHeapDesc.NumDescriptors = FrameCount;
        rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        HR_ERROR_CHECK_CALL(mDevice->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&mRTVHeap)), false, "Failed to create RTV heap!\n");

        D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc{};
        dsvHeapDesc.NumDescriptors = FrameCount;
        dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        HR_ERROR_CHECK_CALL(mDevice->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&mDSVHeap)), false, "Failed to create DSV heap!\n");

        D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc{};
        srvHeapDesc.NumDescriptors = 64;
        srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        HR_ERROR_CHECK_CALL(mDevice->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&mSRVCBVHeap)), false, "Failed to create SRV CBV heap!\n");

        for (UINT i = 0; i < FrameCount; i++)
        {
            D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc{};
            srvHeapDesc.NumDescriptors = 64;
            srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            HR_ERROR_CHECK_CALL(mDevice->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&mSRVCBVFrameHeap[i])), false, "Failed to create SRV CBV frame heap %u!\n", i);
        }

        mRTVDescriptorSize = mDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        mDSVDescriptorSize = mDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        mSRVCBVDescriptorSize = mDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(mRTVHeap->GetCPUDescriptorHandleForHeapStart());
        CD3DX12_CPU_DESCRIPTOR_HANDLE dsvHandle(mDSVHeap->GetCPUDescriptorHandleForHeapStart());
        for (UINT n = 0; n < FrameCount; n++)
        {
            HR_ERROR_CHECK_CALL(mSwapChain->GetBuffer(n, IID_PPV_ARGS(&mRenderTargets[n])), false, "Unabled to get buffer for render target %u\n", n);

            mDevice->CreateRenderTargetView(mRenderTargets[n].Get(), nullptr, rtvHandle);

            {
                CD3DX12_RESOURCE_DESC depthTexDesc{
                    D3D12_RESOURCE_DIMENSION_TEXTURE2D,
                    0,
                    mWidth,
                    mHeight,
                    1,
                    1,
                    DXGI_FORMAT_D32_FLOAT,
                    1,
                    0,
                    D3D12_TEXTURE_LAYOUT_UNKNOWN,
                    D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE
                };

                D3D12_CLEAR_VALUE clearVal;
                clearVal.Format = DXGI_FORMAT_D32_FLOAT;
                clearVal.DepthStencil.Depth = 1.f;
                clearVal.DepthStencil.Stencil = 0;

                HR_ERROR_CHECK_CALL(mDevice->CreateCommittedResource(
                    &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
                    D3D12_HEAP_FLAG_NONE,
                    &depthTexDesc,
                    D3D12_RESOURCE_STATE_DEPTH_WRITE,
                    &clearVal,
                    IID_PPV_ARGS(mDepthStencils[n].GetAddressOf())), false, "Failed to create depth buffer %d\n", n);
            }
                                 
            mDevice->CreateDepthStencilView(mDepthStencils[n].Get(), nullptr, dsvHandle);

            rtvHandle.Offset(1, mRTVDescriptorSize);
            dsvHandle.Offset(1, mDSVDescriptorSize);
        
            HR_ERROR_CHECK_CALL(mDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&mCommandAllocator[n])), false, "failed to create command allocator %u\n", n);
        }

        {
            HR_ERROR_CHECK_CALL(mDevice->CreateCommittedResource(
                &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
                D3D12_HEAP_FLAG_NONE,
                &CD3DX12_RESOURCE_DESC::Buffer(1024 * 120),
                D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr,
                IID_PPV_ARGS(&mConstantBuffer)), false, "Failed to create constant buffer!\n");

            CD3DX12_RANGE readRange(0, 0);
            HR_ERROR_CHECK_CALL(mConstantBuffer->Map(0, &readRange, reinterpret_cast<void**>(&mCBVDataBegin)), false, "Faild to map constant buffer\n");
        }

        return true;
    }
    
    bool loadAssets()
    {
        HR_ERROR_CHECK_CALL(mDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, mCommandAllocator[mFrameIndex].Get(), nullptr, IID_PPV_ARGS(&mCommandList)), false, "Failed to create command list\n");

        if (mSimpleShader->prepare(mDevice.Get()) == false)
        {
            LOG_ERROR("Failed to prepare shader\n");
            return false;
        }

        UINT heapOffset = 0;
        UINT cbDataOffset = 0;

        if (!mShadowMap->prepare(
            mDevice.Get(),
            mCommandQueue.Get(),
            mCommandList.Get(),
            mSRVCBVHeap.Get(),
            heapOffset,
            mConstantBuffer.Get(),
            cbDataOffset,
            mCBVDataBegin,
            FrameCount))
        {
            LOG_ERROR("Failed to prepare shadowmap\n");
            return false;
        }

        for (auto const & model : mModels)
        {
            if (!model->prepare(
                mDevice.Get(),
                mCommandQueue.Get(),
                mCommandList.Get(),
                mSRVCBVHeap.Get(),
                heapOffset,
                mSimpleShader.get(),
                mShadowMap.get(),
                mConstantBuffer.Get(),
                cbDataOffset,
                mCBVDataBegin,
                FrameCount))
            {
                LOG_ERROR("Failed to prepare model\n");
                return false;
            }
        }

        HR_ERROR_CHECK_CALL(mCommandList->Close(), false, "Failed to close commandlist\n");
        ID3D12CommandList* ppCommandLists[] = { mCommandList.Get() };
        mCommandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);

        HR_ERROR_CHECK_CALL(mDevice->CreateFence(mFenceValue[mFrameIndex], D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&mFence)), false, "Failed to create fence\n");

        mFenceValue[mFrameIndex]++;
        mFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (mFenceEvent == nullptr)
        {
            LOG_ERROR("Failed to create fence event\n");
            return false;
        }

        waitForGPU();

        return true;
    }

    void populateShadowCommandList(uint32_t &frameHeapOffset)
    {
        HR_ERROR_CHECK_CALL(mCommandList->Reset(mCommandAllocator[mFrameIndex].Get(), nullptr), void(), "Failed to reset command list\n");

        ID3D12DescriptorHeap* ppHeaps[] = { mSRVCBVFrameHeap[mFrameIndex].Get() };
        mCommandList->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);
        {
            auto pipelineState = mShadowMap->getPipelineState().Get();
            mCommandList->SetPipelineState(pipelineState);
            mCommandList->SetGraphicsRootSignature(mShadowMap->getRootSignature().Get());

            mCommandList->RSSetViewports(1, &mShadowViewport);
            mCommandList->RSSetScissorRects(1, &mShadowScissorRect);

            CD3DX12_CPU_DESCRIPTOR_HANDLE dsvHandle(mShadowMap->getDSVHeap()->GetCPUDescriptorHandleForHeapStart());
            mCommandList->OMSetRenderTargets(0, nullptr, FALSE, &dsvHandle);
            mCommandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

            for (auto const& model : mModels)
            {
                model->updateShadowDescriptors(mDevice.Get(), mCommandList.Get(), mSRVCBVFrameHeap[mFrameIndex].Get(), frameHeapOffset, mFrameIndex);
                mCommandList->ExecuteBundle(model->getShadowBundle().Get());
            }

            mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mShadowMap->getDepthTexture().Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
        }

        HR_ERROR_CHECK_CALL(mCommandList->Close(), void(), "Failed to close command list\n");
    }

    void populateCommandList(uint32_t &frameHeapOffset)
    {
        HR_ERROR_CHECK_CALL(mCommandList->Reset(mCommandAllocator[mFrameIndex].Get(), nullptr), void(), "Failed to reset command list\n");

        ID3D12DescriptorHeap* ppHeaps[] = { mSRVCBVFrameHeap[mFrameIndex].Get() };
        mCommandList->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);
        {
            auto pipelineState = mSimpleShader->getPipelineState().Get();
            mCommandList->SetPipelineState(pipelineState);
            mCommandList->SetGraphicsRootSignature(mSimpleShader->getRootSignature().Get());

            mCommandList->RSSetViewports(1, &mViewport);
            mCommandList->RSSetScissorRects(1, &mScissorRect);

            mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRenderTargets[mFrameIndex].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET));

            CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(mRTVHeap->GetCPUDescriptorHandleForHeapStart(), mFrameIndex, mRTVDescriptorSize);
            CD3DX12_CPU_DESCRIPTOR_HANDLE dsvHandle(mDSVHeap->GetCPUDescriptorHandleForHeapStart(), mFrameIndex, mDSVDescriptorSize);

            mCommandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

            const float clearColor[] = { 0.0f, 0.2f, 0.4f, 1.0f };
            mCommandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
            mCommandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
            
            for (auto const& model : mModels)
            {
                model->updateDescriptors(mDevice.Get(), mCommandList.Get(), mSRVCBVFrameHeap[mFrameIndex].Get(), frameHeapOffset, mFrameIndex);
                mCommandList->ExecuteBundle(model->getBundle().Get());
            }

            mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRenderTargets[mFrameIndex].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT));
            mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mShadowMap->getDepthTexture().Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE));
        }

        HR_ERROR_CHECK_CALL(mCommandList->Close(), void(), "Failed to close command list\n");
    }

    void insertGPUFence()
    {
        HR_ERROR_CHECK_CALL(mCommandQueue->Signal(mFence.Get(), mFenceValue[mFrameIndex]), void(), "Failed to signal command queue!\n");
        HR_ERROR_CHECK_CALL(mCommandQueue->Wait(mFence.Get(), mFenceValue[mFrameIndex]), void(), "Failed to signal command queue!\n");

        mFenceValue[mFrameIndex]++;
    }

    void waitForGPU()
    {
        HR_ERROR_CHECK_CALL(mCommandQueue->Signal(mFence.Get(), mFenceValue[mFrameIndex]), void(), "Failed to signal command queue!\n");

        HR_ERROR_CHECK_CALL(mFence->SetEventOnCompletion(mFenceValue[mFrameIndex], mFenceEvent), void(), "Failed to set event on completion\n");
        WaitForSingleObjectEx(mFenceEvent, INFINITE, FALSE);

        mFenceValue[mFrameIndex]++;
    }

    void MoveToNextFrame()
    {
        const UINT64 currentFenceValue = mFenceValue[mFrameIndex];
        HR_ERROR_CHECK_CALL(mCommandQueue->Signal(mFence.Get(), currentFenceValue), void(), "Failed to signal command queue!\n");

        mFrameIndex = mSwapChain->GetCurrentBackBufferIndex();

        if (mFence->GetCompletedValue() < mFenceValue[mFrameIndex])
        {
            HR_ERROR_CHECK_CALL(mFence->SetEventOnCompletion(mFenceValue[mFrameIndex], mFenceEvent), void(), "Failed to set event on completion\n");
            WaitForSingleObjectEx(mFenceEvent, INFINITE, FALSE);
        }

        mFenceValue[mFrameIndex] = currentFenceValue + 1;
    }

    static const UINT FrameCount{ 2 };

    uint32_t mWidth;
    uint32_t mHeight;
    float mAspectRatio;

    CD3DX12_VIEWPORT mViewport;
    CD3DX12_RECT mScissorRect;

    CD3DX12_VIEWPORT mShadowViewport;
    CD3DX12_RECT mShadowScissorRect;

    ComPtr<ID3D12Device> mDevice;
    ComPtr<ID3D12CommandQueue> mCommandQueue;
    ComPtr<IDXGISwapChain3> mSwapChain;
    ComPtr<ID3D12DescriptorHeap> mRTVHeap;
    ComPtr<ID3D12DescriptorHeap> mDSVHeap;
    ComPtr<ID3D12DescriptorHeap> mSRVCBVHeap;
    ComPtr<ID3D12DescriptorHeap> mSRVCBVFrameHeap[FrameCount];
    ComPtr<ID3D12Resource> mRenderTargets[FrameCount];
    ComPtr<ID3D12Resource> mDepthStencils[FrameCount];
    ComPtr<ID3D12CommandAllocator> mCommandAllocator[FrameCount];
    ComPtr<ID3D12GraphicsCommandList> mCommandList;
    ComPtr<ID3D12Fence> mFence;

    ComPtr<ID3D12Resource> mConstantBuffer;
    UINT8* mCBVDataBegin{ nullptr };

    UINT mFrameIndex;
    UINT mRTVDescriptorSize;
    UINT mDSVDescriptorSize;
    UINT mSRVCBVDescriptorSize;
    UINT64 mFenceValue[FrameCount]{};

    HANDLE mFenceEvent;

    std::vector<std::unique_ptr<Model>> mModels;
    std::unique_ptr<SimpleShader> mSimpleShader;
    std::unique_ptr<ShadowMap> mShadowMap;

    bool mIsInitialized{ false };
};

Renderer* Renderer::create()
{
    assert(gInstance == nullptr);
    gInstance = static_cast<Renderer*>(new D3D12RendererImpl());
    return gInstance;
}

Renderer& Renderer::getInstance()
{
    assert(gInstance);
    return *gInstance;
}

bool Renderer::isCreated()
{
    return gInstance != nullptr;
}

}