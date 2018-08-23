#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN             // Exclude rarely-used stuff from Windows headers.
#endif

#include <assert.h>
#include <stdio.h>
#include <Windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>

#include <wrl.h>

#include "d3dx12.h"
#include "Renderer.h"

#define LOG_ERROR(...) printf(__VA_ARGS__)

using namespace Microsoft::WRL;

namespace HDX
{

static Renderer* gInstance{ nullptr };

class D3D12RendererImpl : public Renderer
{
public:
    void release() final
    {
        waitForPreviousFrame();

        CloseHandle(mFenceEvent);

        assert(gInstance == this);
        gInstance = nullptr;
        delete this;
    }

    bool init(HWND hWnd, uint32_t width, uint32_t height) final
    {
        mWidth = width;
        mHeight = height;

        if (!loadPipeline(hWnd))
        {
            return false;
        }

        if (!loadAssets())
        {
            return false;
        }

        return true;
    }

    void onRender() final
    {
        populateCommandList();

        ID3D12CommandList* ppCommadLists[] = { mCommandList.Get() };
        mCommandQueue->ExecuteCommandLists(_countof(ppCommadLists), ppCommadLists);

        if (FAILED(mSwapChain->Present(1, 0)))
        {
            assert(false);
        }

        waitForPreviousFrame();
    }

private:

    bool loadPipeline(HWND hWnd)
    {
        UINT dxgiFactoryFlag{ 0 };

#ifdef _DEBUG
        ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
        {
            debugController->EnableDebugLayer();
            dxgiFactoryFlag |= DXGI_CREATE_FACTORY_DEBUG;
        }
#endif

        ComPtr<IDXGIFactory4> factory;
        if (FAILED(CreateDXGIFactory2(dxgiFactoryFlag, IID_PPV_ARGS(&factory))))
        {
            LOG_ERROR("Failed to create DXGI factory!\n");
            return false;
        }

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

        if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&mDevice))))
        {
            LOG_ERROR("Failed to create D3D12 Device!\n");
            return false;
        }

        D3D12_COMMAND_QUEUE_DESC queueDesc {};
        queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

        if (FAILED(mDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&mCommandQueue))))
        {
            LOG_ERROR("Failed to create command queue!\n");
            return false;
        }

        DXGI_SWAP_CHAIN_DESC1 swapChainDesc{};
        swapChainDesc.BufferCount = FrameCount;
        swapChainDesc.Width = mWidth;
        swapChainDesc.Height = mHeight;
        swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        swapChainDesc.SampleDesc.Count = 1;

        ComPtr<IDXGISwapChain1> swapChain;
        if (FAILED(factory->CreateSwapChainForHwnd(mCommandQueue.Get(), hWnd, &swapChainDesc, nullptr, nullptr, &swapChain)))
        {
            LOG_ERROR("Failed to create swap chain!\n");
            return false;
        }

        if (FAILED(factory->MakeWindowAssociation(hWnd, DXGI_MWA_NO_ALT_ENTER)))
        {
            LOG_ERROR("Failed to change windows association!\n");
            return false;
        }

        if (FAILED(swapChain.As(&mSwapChain)))
        {
            LOG_ERROR("Failed to cast swap chain!\n");
            return false;
        }

        mFrameIndex = mSwapChain->GetCurrentBackBufferIndex();


        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
        rtvHeapDesc.NumDescriptors = FrameCount;
        rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (FAILED(mDevice->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&mRTVHeap))))
        {
            LOG_ERROR("Failed to create RTV heap!\n");
            return false;
        }

        mRTVDescriptorSize = mDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

        
        CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(mRTVHeap->GetCPUDescriptorHandleForHeapStart());
        for (UINT n = 0; n < FrameCount; n++)
        {
            if (FAILED(mSwapChain->GetBuffer(n, IID_PPV_ARGS(&mRenderTargets[n]))))
            {
                LOG_ERROR("Unabled to get buffer for render target %u\n", n);
                return false;
            }
            mDevice->CreateRenderTargetView(mRenderTargets[n].Get(), nullptr, rtvHandle);
            rtvHandle.Offset(1, mRTVDescriptorSize);
        }

        if (FAILED(mDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&mCommandAllocator))))
        {
            LOG_ERROR("failed to create command allocator\n");
            return false;
        }

        return true;
    }

    bool loadAssets()
    {
        if (FAILED(mDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, mCommandAllocator.Get(), nullptr, IID_PPV_ARGS(&mCommandList))))
        {
            LOG_ERROR("Failed to create command list\n");
            return false;
        }

        if (FAILED(mCommandList->Close()))
        {
            LOG_ERROR("Failed to close command list\n");
            return false;
        }

        if (FAILED(mDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&mFence))))
        {
            LOG_ERROR("Failed to create fence\n");
            return false;
        }

        mFenceValue = 1;
        mFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (mFenceEvent == nullptr)
        {
            LOG_ERROR("Failed to create fence event\n");
            return false;
        }

        return true;
    }

    void populateCommandList()
    {
        if (FAILED(mCommandAllocator->Reset()))
        {
            LOG_ERROR("Failed to reset command allocator\n");
            assert(false);
            return;
        }

        if (FAILED(mCommandList->Reset(mCommandAllocator.Get(), mPipelineState.Get())))
        {
            LOG_ERROR("Failed to reset command list\n");
            assert(false);
            return;
        }

        mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRenderTargets[mFrameIndex].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET));

        CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(mRTVHeap->GetCPUDescriptorHandleForHeapStart(), mFrameIndex, mRTVDescriptorSize);

        const float clearColor[] = { 0.0f, 0.2f, 0.4f, 1.0f };
        mCommandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

        mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRenderTargets[mFrameIndex].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT));

        if (FAILED(mCommandList->Close()))
        {
            LOG_ERROR("Failed to close command list\n");
            assert(false);
            return;
        }
    }

    void waitForPreviousFrame()
    {
        const UINT64 fence = mFenceValue;
        if (FAILED(mCommandQueue->Signal(mFence.Get(), fence)))
        {
            assert(false);
        }
        mFenceValue++;

        if (mFence->GetCompletedValue() < fence)
        {
            if (FAILED(mFence->SetEventOnCompletion(fence, mFenceEvent)))
            {
                assert(false);
            }
            WaitForSingleObject(mFenceEvent, INFINITE);
        }

        mFrameIndex = mSwapChain->GetCurrentBackBufferIndex();
    }

    static const UINT FrameCount{ 2 };

    uint32_t mWidth;
    uint32_t mHeight;

    ComPtr<ID3D12Device> mDevice;
    ComPtr<ID3D12CommandQueue> mCommandQueue;
    ComPtr<IDXGISwapChain3> mSwapChain;
    ComPtr<ID3D12DescriptorHeap> mRTVHeap;
    ComPtr<ID3D12Resource> mRenderTargets[FrameCount];
    ComPtr<ID3D12CommandAllocator> mCommandAllocator;
    ComPtr<ID3D12GraphicsCommandList> mCommandList;
    ComPtr<ID3D12PipelineState> mPipelineState;
    ComPtr<ID3D12Fence> mFence;

    UINT mFrameIndex;
    UINT mRTVDescriptorSize;
    UINT64 mFenceValue;

    HANDLE mFenceEvent;
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

}