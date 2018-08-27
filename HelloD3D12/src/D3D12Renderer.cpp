#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN             // Exclude rarely-used stuff from Windows headers.
#endif

#include <assert.h>
#include <stdio.h>
#include <vector>
#include <Windows.h>

#include <d3d12.h>
#include <D3Dcompiler.h>
#include <DirectXMath.h>
#include <dxgi1_6.h>

#include <wrl.h>

#include "d3dx12.h"
#include "Renderer.h"

#ifdef _DEBUG
#define LOG_ERROR(...) printf(__VA_ARGS__)
#define HR_ERROR_CHECK_CALL(func, ret, ... ) \
        if (FAILED(func)) \
        {   \
            LOG_ERROR(__VA_ARGS__); \
            assert((#func) == "failed"); \
            return ret; \
        } 
#else
#define LOG_ERROR(...) void()
#define HR_ERROR_CHECK_CALL(func, ret, ... ) func
#endif

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
			mConstantBuffer->Unmap(0, nullptr);
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

        D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc{};
        srvHeapDesc.NumDescriptors = 2;
        srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        HR_ERROR_CHECK_CALL(mDevice->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&mSRVCBVHeap)), false, "Failed to create SRV CBV heap!\n");

        mRTVDescriptorSize = mDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        mSRVCBVDescriptorSize = mDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(mRTVHeap->GetCPUDescriptorHandleForHeapStart());
        for (UINT n = 0; n < FrameCount; n++)
        {
            HR_ERROR_CHECK_CALL(mSwapChain->GetBuffer(n, IID_PPV_ARGS(&mRenderTargets[n])), false, "Unabled to get buffer for render target %u\n", n);

            mDevice->CreateRenderTargetView(mRenderTargets[n].Get(), nullptr, rtvHandle);
            rtvHandle.Offset(1, mRTVDescriptorSize);
        }

        HR_ERROR_CHECK_CALL(mDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&mCommandAllocator)), false, "failed to create command allocator\n");
        HR_ERROR_CHECK_CALL(mDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_BUNDLE, IID_PPV_ARGS(&mBundleAllocator)), false, "failed to create bundle allocator\n");
        return true;
    }
    
    bool loadAssets()
    {
        {
            D3D12_FEATURE_DATA_ROOT_SIGNATURE featureData{};

            featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;

            if (FAILED(mDevice->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &featureData, sizeof(featureData))))
            {
                featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_0;
            }

            CD3DX12_DESCRIPTOR_RANGE1 ranges[2];
            ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC);
            ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC);

            CD3DX12_ROOT_PARAMETER1 rootParameters[2];
            rootParameters[0].InitAsDescriptorTable(1, &ranges[0], D3D12_SHADER_VISIBILITY_PIXEL);
            rootParameters[1].InitAsDescriptorTable(1, &ranges[1], D3D12_SHADER_VISIBILITY_VERTEX);

            D3D12_STATIC_SAMPLER_DESC sampler{};
            sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
            sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
            sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
            sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
            sampler.MipLODBias = 0;
            sampler.MaxAnisotropy = 0;
            sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
            sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
            sampler.MinLOD = 0.f;
            sampler.MaxLOD = D3D12_FLOAT32_MAX;
            sampler.ShaderRegister = 0;
            sampler.RegisterSpace = 0;
            sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

            D3D12_ROOT_SIGNATURE_FLAGS rootSignatureFlags =
                D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
                D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

            CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootSignatureDesc;
            rootSignatureDesc.Init_1_1(_countof(rootParameters), rootParameters, 1, &sampler, rootSignatureFlags);

            ComPtr<ID3DBlob> signature;
            ComPtr<ID3DBlob> error;

            HR_ERROR_CHECK_CALL(D3DX12SerializeVersionedRootSignature(&rootSignatureDesc, featureData.HighestVersion, &signature, &error), false, "Failed to serialize root signature\n");
            HR_ERROR_CHECK_CALL(mDevice->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&mRootSignature)), false, "Failed to create root signature\n");
        }

        {
            ComPtr<ID3DBlob> vertexShader;
            ComPtr<ID3DBlob> pixelShader;
            ComPtr<ID3DBlob> errorMsg;
#ifdef _DEBUG
            UINT compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
            UINT compileFlags = 0;
#endif
            const char* shader =
                "cbuffer SceneConstantBuffer : register(b0)\n                      \n"
                "{                                                                 \n"
                "   float4 offset;                                                 \n"
                "}                                                                 \n"
                "                                                                  \n"
                "struct PSInput                                                    \n"
                "{                                                                 \n"
                "	float4 position : SV_POSITION;                                 \n"
                "	float2 uv : TEXCOORD;                                          \n"
                "};                                                                \n"
                "                                                                  \n"
                "Texture2D g_texture : register(t0);                               \n"
                "SamplerState g_sampler : register(s0);                            \n"
                "                                                                  \n"
                "PSInput VSMain(float4 position : POSITION, float2 uv : TEXCOORD)  \n"
                "{                                                                 \n"
                "	PSInput result;                                                \n"
                "                                                                  \n"
                "	result.position = position + offset;                           \n"
                "	result.uv = uv;                                                \n"
                "                                                                  \n"
                "	return result;                                                 \n"
                "}                                                                 \n"
                "                                                                  \n"
                "float4 PSMain(PSInput input) : SV_TARGET                          \n"
                "{                                                                 \n"
                "	return g_texture.Sample(g_sampler, input.uv);                  \n"
                "}                                                                 \n";

            if (FAILED(D3DCompile(shader, strlen(shader) + 1, nullptr, nullptr, nullptr, "VSMain", "vs_5_0", compileFlags, 0, &vertexShader, &errorMsg)))
            {
                if (errorMsg)
                {
                    LOG_ERROR(reinterpret_cast<const char*>(errorMsg->GetBufferPointer()));
                }

                assert(false);
                return false;
            }

            if (FAILED(D3DCompile(shader, strlen(shader) + 1, nullptr, nullptr, nullptr, "PSMain", "ps_5_0", compileFlags, 0, &pixelShader, &errorMsg)))
            {
                if (errorMsg)
                {
                    LOG_ERROR(reinterpret_cast<const char*>(errorMsg->GetBufferPointer()));
                }

                assert(false);
                return false;
            }

            D3D12_INPUT_ELEMENT_DESC inputElementDescs[]
            {
                {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}
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

        {
            Vertex triangleVertices[] =
            {
                { { 0.0f, 0.25f * mAspectRatio, 0.0f }, { 0.5f, 0.0f } },
                { { 0.25f, -0.25f * mAspectRatio, 0.0f }, { 1.f, 1.f } },
                { { -0.25f, -0.25f * mAspectRatio, 0.0f }, { 0.f, 1.f } }
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

        ComPtr<ID3D12Resource> textureUploadHeap; // scope!! Don't destroy it before finishing execute commandqueue.
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

            HR_ERROR_CHECK_CALL(mDevice->CreateCommittedResource(
                &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
                D3D12_HEAP_FLAG_NONE,
                &textureDesc,
                D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr,
                IID_PPV_ARGS(&mTexture)), false, "Failed to create texture!\n");

            const UINT64 uploadBufferSize = GetRequiredIntermediateSize(mTexture.Get(), 0, 1);

            HR_ERROR_CHECK_CALL(mDevice->CreateCommittedResource(
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

            UpdateSubresources(mCommandList.Get(), mTexture.Get(), textureUploadHeap.Get(), 0, 0, 1, &textureData);
            mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mTexture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));

            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Format = textureDesc.Format;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MipLevels = 1;
            mDevice->CreateShaderResourceView(mTexture.Get(), &srvDesc, mSRVCBVHeap->GetCPUDescriptorHandleForHeapStart());
        }

        {
            HR_ERROR_CHECK_CALL(mDevice->CreateCommittedResource(
                &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
                D3D12_HEAP_FLAG_NONE,
                &CD3DX12_RESOURCE_DESC::Buffer(1024 * 64),
                D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr,
                IID_PPV_ARGS(&mConstantBuffer)), false, "Failed to create constant buffer!\n");

            D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc{};
            cbvDesc.BufferLocation = mConstantBuffer->GetGPUVirtualAddress();
            cbvDesc.SizeInBytes = (sizeof(SceneConstantBuffer) + 255) & ~255;

            CD3DX12_CPU_DESCRIPTOR_HANDLE cbvHandle(mSRVCBVHeap->GetCPUDescriptorHandleForHeapStart());
            cbvHandle.Offset(1, mSRVCBVDescriptorSize);

            mDevice->CreateConstantBufferView(&cbvDesc, cbvHandle);

            CD3DX12_RANGE readRange(0, 0);
            HR_ERROR_CHECK_CALL(mConstantBuffer->Map(0, &readRange, reinterpret_cast<void**>(&mCBVDataBegin)), false, "Faild to map constant buffer\n");
            memcpy(mCBVDataBegin, &mConstantBufferData, sizeof(mConstantBufferData));
        }

        HR_ERROR_CHECK_CALL(mCommandList->Close(), false, "Failed to close commandlist\n");
        ID3D12CommandList* ppCommandLists[] = { mCommandList.Get() };
        mCommandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);

        {
            HR_ERROR_CHECK_CALL(mDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_BUNDLE, mBundleAllocator.Get(), mPipelineState.Get(), IID_PPV_ARGS(&mBundle)), false, "Failed to create bundle\n");
            mBundle->SetGraphicsRootSignature(mRootSignature.Get());
            mBundle->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            mBundle->IASetVertexBuffers(0, 1, &mVertexBufferView);
            mBundle->DrawInstanced(3, 1, 0, 0);
            HR_ERROR_CHECK_CALL(mBundle->Close(), false, "Failed to close bundle\n");
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

        ID3D12DescriptorHeap* ppHeaps[] = { mSRVCBVHeap.Get() };
        mCommandList->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);

        mCommandList->SetGraphicsRootDescriptorTable(0, mSRVCBVHeap->GetGPUDescriptorHandleForHeapStart());

        CD3DX12_GPU_DESCRIPTOR_HANDLE cbvHandle(mSRVCBVHeap->GetGPUDescriptorHandleForHeapStart());
        cbvHandle.Offset(1, mSRVCBVDescriptorSize);
        
        mCommandList->SetGraphicsRootDescriptorTable(1, cbvHandle);
        mCommandList->RSSetViewports(1, &mViewport);
        mCommandList->RSSetScissorRects(1, &mScissorRect);

        mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRenderTargets[mFrameIndex].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET));

        CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(mRTVHeap->GetCPUDescriptorHandleForHeapStart(), mFrameIndex, mRTVDescriptorSize);
        mCommandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

        const float clearColor[] = { 0.0f, 0.2f, 0.4f, 1.0f };
        mCommandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
        
        mCommandList->ExecuteBundle(mBundle.Get());

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
    static const UINT TextureWidth{ 256 };
    static const UINT TextureHeight{ 256 };
    static const UINT TexturePixelSize{ 4 };

    struct Vertex
    {
        XMFLOAT3 position;
        XMFLOAT2 uv;
    };

    struct SceneConstantBuffer
    {
        XMFLOAT4 offset;
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
    ComPtr<ID3D12DescriptorHeap> mSRVCBVHeap;
    ComPtr<ID3D12Resource> mRenderTargets[FrameCount];
    ComPtr<ID3D12CommandAllocator> mCommandAllocator;
    ComPtr<ID3D12GraphicsCommandList> mCommandList;
    ComPtr<ID3D12PipelineState> mPipelineState;
    ComPtr<ID3D12Fence> mFence;
    ComPtr<ID3D12RootSignature> mRootSignature;
    ComPtr<ID3D12Resource> mVertexBuffer;
    D3D12_VERTEX_BUFFER_VIEW mVertexBufferView;
    ComPtr<ID3D12Resource> mTexture;
    ComPtr<ID3D12CommandAllocator> mBundleAllocator;
    ComPtr<ID3D12GraphicsCommandList> mBundle;
    ComPtr<ID3D12Resource> mConstantBuffer;
    UINT8* mCBVDataBegin{ nullptr };

    UINT mFrameIndex;
    UINT mRTVDescriptorSize;
    UINT mSRVCBVDescriptorSize;
    UINT64 mFenceValue;

    HANDLE mFenceEvent;

    SceneConstantBuffer mConstantBufferData{};

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