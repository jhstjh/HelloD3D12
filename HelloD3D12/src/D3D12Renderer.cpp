#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN             // Exclude rarely-used stuff from Windows headers.
#endif

#include <assert.h>
#include <stdio.h>
#include <Windows.h>

#include <d3d12.h>
#include <D3Dcompiler.h>
#include <DirectXMath.h>
#include <dxgi1_6.h>

#include <wrl.h>

#include "d3dx12.h"
#include "Renderer.h"

#define LOG_ERROR(...) printf(__VA_ARGS__)

#define HR_ERROR_CHECK_CALL(func, ret, ... ) \
        if (FAILED(func)) \
        {   \
            LOG_ERROR(__VA_ARGS__); \
            assert((#func) == "failed"); \
            return ret; \
        } 

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
            waitForPreviousFrame();
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

        mRTVDescriptorSize = mDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  
        CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(mRTVHeap->GetCPUDescriptorHandleForHeapStart());
        for (UINT n = 0; n < FrameCount; n++)
        {
            HR_ERROR_CHECK_CALL(mSwapChain->GetBuffer(n, IID_PPV_ARGS(&mRenderTargets[n])), false, "Unabled to get buffer for render target %u\n", n);

            mDevice->CreateRenderTargetView(mRenderTargets[n].Get(), nullptr, rtvHandle);
            rtvHandle.Offset(1, mRTVDescriptorSize);
        }

        HR_ERROR_CHECK_CALL(mDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&mCommandAllocator)), false, "failed to create command allocator\n");
        return true;
    }
    
    bool loadAssets()
    {
        {
            CD3DX12_ROOT_SIGNATURE_DESC rootSignatureDesc;
            rootSignatureDesc.Init(0, nullptr, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

            ComPtr<ID3DBlob> signature;
            ComPtr<ID3DBlob> error;

            HR_ERROR_CHECK_CALL(D3D12SerializeRootSignature(&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error), false, "Failed to serialize root signature\n");
            HR_ERROR_CHECK_CALL(mDevice->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&mRootSignature)), false, "Failed to create root signature\n");
        }

        {
            ComPtr<ID3DBlob> vertexShader;
            ComPtr<ID3DBlob> pixelShader;

#ifdef _DEBUG
            UINT compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
            UINT compileFlags = 0;
#endif
            const char* shader =
                "struct PSInput                                                    \n"
                "{                                                                 \n"
                "	float4 position : SV_POSITION;                                 \n"
                "	float4 color : COLOR;                                          \n"
                "};                                                                \n"
                "                                                                  \n"
                "PSInput VSMain(float4 position : POSITION, float4 color : COLOR)  \n"
                "{                                                                 \n"
                "	PSInput result;                                                \n"
                "                                                                  \n"
                "	result.position = position;                                    \n"
                "	result.color = color;                                          \n"
                "                                                                  \n"
                "	return result;                                                 \n"
                "}                                                                 \n"
                "                                                                  \n"
                "float4 PSMain(PSInput input) : SV_TARGET                          \n"
                "{                                                                 \n"
                "	return input.color;                                            \n"
                "}                                                                 \n";

            HR_ERROR_CHECK_CALL(D3DCompile(shader, strlen(shader) + 1, nullptr, nullptr, nullptr, "VSMain", "vs_5_0", compileFlags, 0, &vertexShader, nullptr), false, "Failed to create vertex shader\n");
            HR_ERROR_CHECK_CALL(D3DCompile(shader, strlen(shader) + 1, nullptr, nullptr, nullptr, "PSMain", "ps_5_0", compileFlags, 0, &pixelShader, nullptr), false, "Failed to create pixel shader\n");

            D3D12_INPUT_ELEMENT_DESC inputElementDescs[]
            {
                {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}
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

            HR_ERROR_CHECK_CALL(mDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&mPipelineState)), false, "Failed to create graphics pipeline state\n");
        }

        HR_ERROR_CHECK_CALL(mDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, mCommandAllocator.Get(), nullptr, IID_PPV_ARGS(&mCommandList)), false, "Failed to create command list\n");
        HR_ERROR_CHECK_CALL(mCommandList->Close(), false, "Failed to close command list\n");

        {
            Vertex triangleVertices[] =
            {
                { { 0.0f, 0.25f * mAspectRatio, 0.0f }, { 1.0f, 0.0f, 0.0f, 1.0f } },
                { { 0.25f, -0.25f * mAspectRatio, 0.0f }, { 0.0f, 1.0f, 0.0f, 1.0f } },
                { { -0.25f, -0.25f * mAspectRatio, 0.0f }, { 0.0f, 0.0f, 1.0f, 1.0f } }
            };

            const UINT vertexBufferSize = sizeof(triangleVertices);

            HR_ERROR_CHECK_CALL(mDevice->CreateCommittedResource(
                &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
                D3D12_HEAP_FLAG_NONE,
                &CD3DX12_RESOURCE_DESC::Buffer(vertexBufferSize),
                D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr,
                IID_PPV_ARGS(&mVertexBuffer)), 
                false, "Failed to create committed resource\n");

            UINT8* pVertexDataBegin;
            CD3DX12_RANGE readRange(0, 0);
            HR_ERROR_CHECK_CALL(mVertexBuffer->Map(0, &readRange, reinterpret_cast<void**>(&pVertexDataBegin)), false, "Failed to map vertex buffer data\n");

            memcpy(pVertexDataBegin, triangleVertices, sizeof(triangleVertices));
            mVertexBuffer->Unmap(0, nullptr);

            mVertexBufferView.BufferLocation = mVertexBuffer->GetGPUVirtualAddress();
            mVertexBufferView.StrideInBytes = sizeof(Vertex);
            mVertexBufferView.SizeInBytes = vertexBufferSize;
        }

        HR_ERROR_CHECK_CALL(mDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&mFence)), false, "Failed to create fence\n");

        mFenceValue = 1;
        mFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (mFenceEvent == nullptr)
        {
            LOG_ERROR("Failed to create fence event\n");
            return false;
        }

        waitForPreviousFrame();

        return true;
    }

    void populateCommandList()
    {
        HR_ERROR_CHECK_CALL(mCommandAllocator->Reset(), void(), "Failed to reset command allocator\n");
        HR_ERROR_CHECK_CALL(mCommandList->Reset(mCommandAllocator.Get(), mPipelineState.Get()), void(), "Failed to reset command list\n");

        mCommandList->SetGraphicsRootSignature(mRootSignature.Get());
        mCommandList->RSSetViewports(1, &mViewport);
        mCommandList->RSSetScissorRects(1, &mScissorRect);

        mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRenderTargets[mFrameIndex].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET));

        CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(mRTVHeap->GetCPUDescriptorHandleForHeapStart(), mFrameIndex, mRTVDescriptorSize);
        mCommandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

        const float clearColor[] = { 0.0f, 0.2f, 0.4f, 1.0f };
        mCommandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
        mCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        mCommandList->IASetVertexBuffers(0, 1, &mVertexBufferView);
        mCommandList->DrawInstanced(3, 1, 0, 0);

        mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRenderTargets[mFrameIndex].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT));

        HR_ERROR_CHECK_CALL(mCommandList->Close(), void(), "Failed to close command list\n");
    }

    void waitForPreviousFrame()
    {
        const UINT64 fence = mFenceValue;
        HR_ERROR_CHECK_CALL(mCommandQueue->Signal(mFence.Get(), fence), void(), "Failed to signal command queue!\n");

        mFenceValue++;

        if (mFence->GetCompletedValue() < fence)
        {
            HR_ERROR_CHECK_CALL(mFence->SetEventOnCompletion(fence, mFenceEvent), void(), "Failed to set event on completion\n");
            WaitForSingleObject(mFenceEvent, INFINITE);
        }

        mFrameIndex = mSwapChain->GetCurrentBackBufferIndex();
    }

    static const UINT FrameCount{ 2 };

    struct Vertex
    {
        XMFLOAT3 position;
        XMFLOAT4 color;
    };

    uint32_t mWidth;
    uint32_t mHeight;
    float mAspectRatio;

    CD3DX12_VIEWPORT mViewport;
    CD3DX12_RECT mScissorRect;

    ComPtr<ID3D12Device> mDevice;
    ComPtr<ID3D12CommandQueue> mCommandQueue;
    ComPtr<IDXGISwapChain3> mSwapChain;
    ComPtr<ID3D12DescriptorHeap> mRTVHeap;
    ComPtr<ID3D12Resource> mRenderTargets[FrameCount];
    ComPtr<ID3D12CommandAllocator> mCommandAllocator;
    ComPtr<ID3D12GraphicsCommandList> mCommandList;
    ComPtr<ID3D12PipelineState> mPipelineState;
    ComPtr<ID3D12Fence> mFence;
    ComPtr<ID3D12RootSignature> mRootSignature;
    ComPtr<ID3D12Resource> mVertexBuffer;
    D3D12_VERTEX_BUFFER_VIEW mVertexBufferView;

    UINT mFrameIndex;
    UINT mRTVDescriptorSize;
    UINT64 mFenceValue;

    HANDLE mFenceEvent;

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