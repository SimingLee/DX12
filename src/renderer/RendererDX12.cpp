#include "renderer/RendererDX12.h"

#include "core/Log.h"
#include "platform/Window.h"

#include "imgui.h"
#include "backends/imgui_impl_dx12.h"

#include <DirectXMath.h>
#include <d3d12sdklayers.h>
#include <wincodec.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <cmath>
#include <fstream>
#include <cstring>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <stdexcept>
#include <vector>

using namespace DirectX;
using Microsoft::WRL::ComPtr;

namespace engine
{
namespace
{
constexpr DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D32_FLOAT;

void ThrowIfFailed(HRESULT hr, const char* message)
{
    if (FAILED(hr))
    {
        throw std::runtime_error(message);
    }
}

std::string Narrow(const std::wstring& value)
{
    if (value.empty())
    {
        return {};
    }

    const int size = ::WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}
} // namespace

RendererDX12::~RendererDX12()
{
    Shutdown();
}

bool RendererDX12::Initialize(Window& window)
{
    window_ = &window;
    width_ = std::max(window.Width(), 1u);
    height_ = std::max(window.Height(), 1u);

    try
    {
#if defined(_DEBUG)
        if (ComPtr<ID3D12Debug> debugController; SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
        {
            debugController->EnableDebugLayer();
        }

        if (ComPtr<ID3D12DeviceRemovedExtendedDataSettings> dredSettings;
            SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dredSettings))))
        {
            dredSettings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            dredSettings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        }
#endif

        if (!InitializeDeviceResources())
        {
            return false;
        }
        if (!CreateSwapChainResources())
        {
            return false;
        }
        if (!CreateRootSignatureAndPipeline())
        {
            return false;
        }
        if (!CreateGeometry())
        {
            return false;
        }
        if (!CreateConstantBuffer())
        {
            return false;
        }
        if (!CreateCheckerboardTexture())
        {
            return false;
        }

        initialized_ = true;
        Log(LogLevel::Info, "DX12 renderer initialized successfully.");
        return true;
    }
    catch (const std::exception& exception)
    {
        Log(LogLevel::Error, exception.what());
        Shutdown();
        return false;
    }
}

bool RendererDX12::LoadModelFromFile(const std::wstring& path)
{
    try
    {
        std::vector<Vertex> vertices;
        std::vector<uint16_t> indices;
        if (!LoadObjMesh(path, vertices, indices))
        {
            return false;
        }

        if (!CreateGeometryBuffers(vertices, indices, "external model"))
        {
            return false;
        }

        loadedModelPath_ = path;
        return true;
    }
    catch (const std::exception& exception)
    {
        Log(LogLevel::Error, exception.what());
        return false;
    }
}

bool RendererDX12::LoadTextureFromFile(const std::wstring& path)
{
    try
    {
        ComPtr<IWICImagingFactory> factory;
        ThrowIfFailed(::CoCreateInstance(
            CLSID_WICImagingFactory,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&factory)),
            "Failed to create WIC factory.");

        ComPtr<IWICBitmapDecoder> decoder;
        ThrowIfFailed(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder),
            "Failed to create image decoder.");

        ComPtr<IWICBitmapFrameDecode> frame;
        ThrowIfFailed(decoder->GetFrame(0, &frame), "Failed to read image frame.");

        UINT width = 0;
        UINT height = 0;
        ThrowIfFailed(frame->GetSize(&width, &height), "Failed to query image size.");

        ComPtr<IWICFormatConverter> converter;
        ThrowIfFailed(factory->CreateFormatConverter(&converter), "Failed to create WIC format converter.");
        ThrowIfFailed(
            converter->Initialize(
                frame.Get(),
                GUID_WICPixelFormat32bppRGBA,
                WICBitmapDitherTypeNone,
                nullptr,
                0.0,
                WICBitmapPaletteTypeCustom),
            "Failed to convert image to RGBA8.");

        std::vector<uint8_t> rgba(width * height * 4u);
        ThrowIfFailed(
            converter->CopyPixels(nullptr, width * 4u, static_cast<UINT>(rgba.size()), rgba.data()),
            "Failed to copy image pixels.");

        if (!CreateTextureFromRGBA(width, height, rgba, "external texture"))
        {
            return false;
        }

        loadedTexturePath_ = path;
        return true;
    }
    catch (const std::exception& exception)
    {
        Log(LogLevel::Error, exception.what());
        return false;
    }
}

void RendererDX12::Resize(uint32_t width, uint32_t height)
{
    if (!initialized_ || width == 0 || height == 0)
    {
        width_ = width;
        height_ = height;
        return;
    }

    width_ = width;
    height_ = height;

    std::ostringstream resizeStream;
    resizeStream << "Resizing swap chain to " << width_ << "x" << height_;
    Log(LogLevel::Info, resizeStream.str());

    WaitForGpu();

    for (auto& renderTarget : renderTargets_)
    {
        renderTarget.Reset();
    }
    depthStencil_.Reset();

    DXGI_SWAP_CHAIN_DESC swapChainDesc{};
    ThrowIfFailed(swapChain_->GetDesc(&swapChainDesc), "Failed to read swap chain description.");
    ThrowIfFailed(
        swapChain_->ResizeBuffers(kFrameCount, width_, height_, swapChainDesc.BufferDesc.Format, swapChainDesc.Flags),
        "Failed to resize swap chain buffers.");

    frameIndex_ = swapChain_->GetCurrentBackBufferIndex();
    RecreateRenderTargets();
    ThrowIfFailed(CreateDepthStencil(width_, height_) ? S_OK : E_FAIL, "Failed to recreate depth stencil.");
}

void RendererDX12::BeginImGuiFrame()
{
    if (!imguiInitialized_)
    {
        return;
    }

    ImGui_ImplDX12_NewFrame();
}

void RendererDX12::Render(const Scene& scene, const Camera& camera, ImDrawData* imguiDrawData)
{
    if (!initialized_ || width_ == 0 || height_ == 0)
    {
        return;
    }

    UpdateConstants(scene, camera);
    PopulateCommandList(imguiDrawData);

    ID3D12CommandList* commandLists[] = {commandList_.Get()};
    commandQueue_->ExecuteCommandLists(1, commandLists);

    ThrowIfFailed(swapChain_->Present(1, 0), "Failed to present swap chain.");
    MoveToNextFrame();
}

void RendererDX12::Shutdown()
{
    if (!initialized_ && fenceEvent_ == nullptr)
    {
        return;
    }

    if (device_ != nullptr && commandQueue_ != nullptr)
    {
        WaitForGpu();
    }

    if (fenceEvent_ != nullptr)
    {
        ::CloseHandle(fenceEvent_);
        fenceEvent_ = nullptr;
    }

    if (imguiInitialized_)
    {
        ImGui_ImplDX12_Shutdown();
        imguiInitialized_ = false;
    }

    mappedConstants_ = nullptr;
    textureUpload_.Reset();
    texture_.Reset();
    depthStencil_.Reset();
    constantBuffer_.Reset();
    indexBuffer_.Reset();
    vertexBuffer_.Reset();
    pipelineState_.Reset();
    rootSignature_.Reset();
    for (auto& renderTarget : renderTargets_)
    {
        renderTarget.Reset();
    }
    for (auto& allocator : commandAllocators_)
    {
        allocator.Reset();
    }

    commandList_.Reset();
    fence_.Reset();
    imguiSrvHeap_.Reset();
    cbvSrvHeap_.Reset();
    dsvHeap_.Reset();
    rtvHeap_.Reset();
    swapChain_.Reset();
    commandQueue_.Reset();
    device_.Reset();
    factory_.Reset();
    initialized_ = false;
}

bool RendererDX12::InitializeDeviceResources()
{
    UINT factoryFlags = 0;
#if defined(_DEBUG)
    factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
#endif

    ThrowIfFailed(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&factory_)), "Failed to create DXGI factory.");

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT adapterIndex = 0;
         factory_->EnumAdapterByGpuPreference(adapterIndex, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND;
         ++adapterIndex)
    {
        DXGI_ADAPTER_DESC1 description{};
        adapter->GetDesc1(&description);
        if (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
        {
            adapter.Reset();
            continue;
        }

        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device_))))
        {
            Log(LogLevel::Info, "Selected hardware adapter: " + Narrow(description.Description));
            break;
        }

        adapter.Reset();
    }

    if (device_ == nullptr)
    {
        ThrowIfFailed(factory_->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "Failed to enumerate WARP adapter.");
        ThrowIfFailed(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device_)), "Failed to create WARP device.");
        Log(LogLevel::Warning, "Falling back to WARP adapter.");
    }

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ThrowIfFailed(device_->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&commandQueue_)), "Failed to create command queue.");

    for (uint32_t frame = 0; frame < kFrameCount; ++frame)
    {
        ThrowIfFailed(
            device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&commandAllocators_[frame])),
            "Failed to create command allocator.");
    }

    ThrowIfFailed(
        device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, commandAllocators_[0].Get(), nullptr, IID_PPV_ARGS(&commandList_)),
        "Failed to create command list.");
    ThrowIfFailed(commandList_->Close(), "Failed to close initial command list.");

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
    rtvHeapDesc.NumDescriptors = kFrameCount;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    ThrowIfFailed(device_->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&rtvHeap_)), "Failed to create RTV heap.");

    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc{};
    dsvHeapDesc.NumDescriptors = 1;
    dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    ThrowIfFailed(device_->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&dsvHeap_)), "Failed to create DSV heap.");

    D3D12_DESCRIPTOR_HEAP_DESC cbvSrvHeapDesc{};
    cbvSrvHeapDesc.NumDescriptors = 2;
    cbvSrvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    cbvSrvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(device_->CreateDescriptorHeap(&cbvSrvHeapDesc, IID_PPV_ARGS(&cbvSrvHeap_)), "Failed to create CBV/SRV heap.");

    D3D12_DESCRIPTOR_HEAP_DESC imguiHeapDesc{};
    imguiHeapDesc.NumDescriptors = 1;
    imguiHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    imguiHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(device_->CreateDescriptorHeap(&imguiHeapDesc, IID_PPV_ARGS(&imguiSrvHeap_)), "Failed to create ImGui SRV heap.");

    rtvDescriptorSize_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    cbvSrvDescriptorSize_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    ThrowIfFailed(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)), "Failed to create fence.");
    fenceEvent_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (fenceEvent_ == nullptr)
    {
        ThrowIfFailed(HRESULT_FROM_WIN32(::GetLastError()), "Failed to create fence event.");
    }

    fenceValues_.fill(0);
    Log(LogLevel::Info, "Created DX12 device, command queue, descriptor heaps, and fence.");
    return true;
}

bool RendererDX12::CreateSwapChainResources()
{
    DXGI_SWAP_CHAIN_DESC1 swapChainDesc{};
    swapChainDesc.Width = width_;
    swapChainDesc.Height = height_;
    swapChainDesc.Format = kBackBufferFormat;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.BufferCount = kFrameCount;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.Scaling = DXGI_SCALING_STRETCH;

    ComPtr<IDXGISwapChain1> swapChain;
    ThrowIfFailed(
        factory_->CreateSwapChainForHwnd(commandQueue_.Get(), window_->Handle(), &swapChainDesc, nullptr, nullptr, &swapChain),
        "Failed to create swap chain.");
    ThrowIfFailed(factory_->MakeWindowAssociation(window_->Handle(), DXGI_MWA_NO_ALT_ENTER), "Failed to configure DXGI window association.");
    ThrowIfFailed(swapChain.As(&swapChain_), "Failed to query swap chain interface.");

    frameIndex_ = swapChain_->GetCurrentBackBufferIndex();
    RecreateRenderTargets();
    Log(LogLevel::Info, "Swap chain and render targets created.");
    return CreateDepthStencil(width_, height_);
}

bool RendererDX12::CreateDepthStencil(uint32_t width, uint32_t height)
{
    D3D12_RESOURCE_DESC depthDesc{};
    depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depthDesc.Width = width;
    depthDesc.Height = height;
    depthDesc.DepthOrArraySize = 1;
    depthDesc.MipLevels = 1;
    depthDesc.Format = kDepthFormat;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clearValue{};
    clearValue.Format = kDepthFormat;
    clearValue.DepthStencil.Depth = 1.0f;

    D3D12_HEAP_PROPERTIES heapProperties{};
    heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;

    ThrowIfFailed(
        device_->CreateCommittedResource(
            &heapProperties,
            D3D12_HEAP_FLAG_NONE,
            &depthDesc,
            D3D12_RESOURCE_STATE_DEPTH_WRITE,
            &clearValue,
            IID_PPV_ARGS(&depthStencil_)),
        "Failed to create depth stencil resource.");

    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
    dsvDesc.Format = kDepthFormat;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device_->CreateDepthStencilView(depthStencil_.Get(), &dsvDesc, dsvHeap_->GetCPUDescriptorHandleForHeapStart());

    viewport_ = {0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
    scissorRect_ = {0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    return true;
}

bool RendererDX12::CreateRootSignatureAndPipeline()
{
    const std::wstring shaderDir = L"" ENGINE_SHADER_DIR;
    const auto vertexShader = LoadFileBytes((shaderDir + L"/basic_vs.cso").c_str());
    const auto pixelShader = LoadFileBytes((shaderDir + L"/basic_ps.cso").c_str());

    D3D12_DESCRIPTOR_RANGE1 descriptorRange{};
    descriptorRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    descriptorRange.NumDescriptors = 1;
    descriptorRange.BaseShaderRegister = 0;
    descriptorRange.RegisterSpace = 0;
    descriptorRange.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC;

    std::array<D3D12_ROOT_PARAMETER1, 2> rootParameters{};
    rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParameters[0].Descriptor.ShaderRegister = 0;
    rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rootParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[1].DescriptorTable.NumDescriptorRanges = 1;
    rootParameters[1].DescriptorTable.pDescriptorRanges = &descriptorRange;
    rootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.ShaderRegister = 0;
    sampler.RegisterSpace = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC rootSignatureDesc{};
    rootSignatureDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    rootSignatureDesc.Desc_1_1.NumParameters = static_cast<UINT>(rootParameters.size());
    rootSignatureDesc.Desc_1_1.pParameters = rootParameters.data();
    rootSignatureDesc.Desc_1_1.NumStaticSamplers = 1;
    rootSignatureDesc.Desc_1_1.pStaticSamplers = &sampler;
    rootSignatureDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> serializedRootSignature;
    ComPtr<ID3DBlob> errorBlob;
    ThrowIfFailed(
        D3D12SerializeVersionedRootSignature(&rootSignatureDesc, &serializedRootSignature, &errorBlob),
        "Failed to serialize root signature.");
    ThrowIfFailed(
        device_->CreateRootSignature(
            0,
            serializedRootSignature->GetBufferPointer(),
            serializedRootSignature->GetBufferSize(),
            IID_PPV_ARGS(&rootSignature_)),
        "Failed to create root signature.");

    const D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_RASTERIZER_DESC rasterizerDesc{};
    rasterizerDesc.FillMode = D3D12_FILL_MODE_SOLID;
    rasterizerDesc.CullMode = D3D12_CULL_MODE_BACK;
    rasterizerDesc.FrontCounterClockwise = FALSE;
    rasterizerDesc.DepthClipEnable = TRUE;

    D3D12_BLEND_DESC blendDesc{};
    blendDesc.AlphaToCoverageEnable = FALSE;
    blendDesc.IndependentBlendEnable = FALSE;
    const D3D12_RENDER_TARGET_BLEND_DESC defaultRenderTargetBlendDesc = {
        FALSE, FALSE,
        D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
        D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
        D3D12_LOGIC_OP_NOOP, D3D12_COLOR_WRITE_ENABLE_ALL};
    blendDesc.RenderTarget[0] = defaultRenderTargetBlendDesc;

    D3D12_DEPTH_STENCIL_DESC depthStencilDesc{};
    depthStencilDesc.DepthEnable = TRUE;
    depthStencilDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    depthStencilDesc.DepthFunc = D3D12_COMPARISON_FUNC_LESS;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipelineDesc{};
    pipelineDesc.pRootSignature = rootSignature_.Get();
    pipelineDesc.VS = {vertexShader.data(), vertexShader.size()};
    pipelineDesc.PS = {pixelShader.data(), pixelShader.size()};
    pipelineDesc.BlendState = blendDesc;
    pipelineDesc.SampleMask = UINT_MAX;
    pipelineDesc.RasterizerState = rasterizerDesc;
    pipelineDesc.DepthStencilState = depthStencilDesc;
    pipelineDesc.InputLayout = {inputLayout, static_cast<UINT>(std::size(inputLayout))};
    pipelineDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pipelineDesc.NumRenderTargets = 1;
    pipelineDesc.RTVFormats[0] = kBackBufferFormat;
    pipelineDesc.DSVFormat = kDepthFormat;
    pipelineDesc.SampleDesc.Count = 1;

    ThrowIfFailed(device_->CreateGraphicsPipelineState(&pipelineDesc, IID_PPV_ARGS(&pipelineState_)), "Failed to create graphics pipeline state.");
    Log(LogLevel::Info, "Root signature and graphics pipeline created.");
    return true;
}

bool RendererDX12::InitializeImGui()
{
    ImGui_ImplDX12_InitInfo initInfo{};
    initInfo.Device = device_.Get();
    initInfo.CommandQueue = commandQueue_.Get();
    initInfo.NumFramesInFlight = static_cast<int>(kFrameCount);
    initInfo.RTVFormat = kBackBufferFormat;
    initInfo.DSVFormat = kDepthFormat;
    initInfo.SrvDescriptorHeap = imguiSrvHeap_.Get();
    initInfo.UserData = this;
    initInfo.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* cpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE* gpuHandle) {
        auto* renderer = static_cast<RendererDX12*>(info->UserData);
        if (!renderer->AllocateImGuiDescriptor(cpuHandle, gpuHandle))
        {
            throw std::runtime_error("Failed to allocate ImGui descriptor.");
        }
    };
    initInfo.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle) {
        auto* renderer = static_cast<RendererDX12*>(info->UserData);
        renderer->FreeImGuiDescriptor(cpuHandle, gpuHandle);
    };

    if (!ImGui_ImplDX12_Init(&initInfo))
    {
        Log(LogLevel::Error, "Failed to initialize ImGui DX12 backend.");
        return false;
    }

    imguiInitialized_ = true;
    Log(LogLevel::Info, "ImGui DX12 backend initialized.");
    return true;
}

bool RendererDX12::CreateGeometry()
{
    const std::vector<Vertex> vertices = {{
        {{-1.0f, -1.0f, -1.0f}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f}},
        {{1.0f, -1.0f, -1.0f}, {0.0f, 0.0f, -1.0f}, {1.0f, 1.0f}},
        {{1.0f, 1.0f, -1.0f}, {0.0f, 0.0f, -1.0f}, {1.0f, 0.0f}},
        {{-1.0f, 1.0f, -1.0f}, {0.0f, 0.0f, -1.0f}, {0.0f, 0.0f}},
        {{-1.0f, -1.0f, 1.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}},
        {{-1.0f, 1.0f, 1.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}},
        {{1.0f, 1.0f, 1.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}},
        {{1.0f, -1.0f, 1.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}},
        {{-1.0f, 1.0f, -1.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f}},
        {{1.0f, 1.0f, -1.0f}, {0.0f, 1.0f, 0.0f}, {1.0f, 1.0f}},
        {{1.0f, 1.0f, 1.0f}, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f}},
        {{-1.0f, 1.0f, 1.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f}},
        {{-1.0f, -1.0f, -1.0f}, {0.0f, -1.0f, 0.0f}, {1.0f, 1.0f}},
        {{-1.0f, -1.0f, 1.0f}, {0.0f, -1.0f, 0.0f}, {1.0f, 0.0f}},
        {{1.0f, -1.0f, 1.0f}, {0.0f, -1.0f, 0.0f}, {0.0f, 0.0f}},
        {{1.0f, -1.0f, -1.0f}, {0.0f, -1.0f, 0.0f}, {0.0f, 1.0f}},
        {{-1.0f, -1.0f, 1.0f}, {-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f}},
        {{-1.0f, -1.0f, -1.0f}, {-1.0f, 0.0f, 0.0f}, {1.0f, 1.0f}},
        {{-1.0f, 1.0f, -1.0f}, {-1.0f, 0.0f, 0.0f}, {1.0f, 0.0f}},
        {{-1.0f, 1.0f, 1.0f}, {-1.0f, 0.0f, 0.0f}, {0.0f, 0.0f}},
        {{1.0f, -1.0f, -1.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f}},
        {{1.0f, -1.0f, 1.0f}, {1.0f, 0.0f, 0.0f}, {1.0f, 1.0f}},
        {{1.0f, 1.0f, 1.0f}, {1.0f, 0.0f, 0.0f}, {1.0f, 0.0f}},
        {{1.0f, 1.0f, -1.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f}},
    }};

    const std::vector<uint16_t> indices = {
        0, 1, 2, 0, 2, 3,
        4, 5, 6, 4, 6, 7,
        8, 9, 10, 8, 10, 11,
        12, 13, 14, 12, 14, 15,
        16, 17, 18, 16, 18, 19,
        20, 21, 22, 20, 22, 23};
    return CreateGeometryBuffers(vertices, indices, "default cube");
}

bool RendererDX12::AllocateImGuiDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE* cpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE* gpuHandle)
{
    if (imguiDescriptorAllocated_)
    {
        return false;
    }

    *cpuHandle = imguiSrvHeap_->GetCPUDescriptorHandleForHeapStart();
    *gpuHandle = imguiSrvHeap_->GetGPUDescriptorHandleForHeapStart();
    imguiDescriptorAllocated_ = true;
    return true;
}

void RendererDX12::FreeImGuiDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_GPU_DESCRIPTOR_HANDLE)
{
    imguiDescriptorAllocated_ = false;
}

bool RendererDX12::CreateConstantBuffer()
{
    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC bufferDesc{};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = kConstantBufferSize;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ThrowIfFailed(
        device_->CreateCommittedResource(
            &uploadHeap,
            D3D12_HEAP_FLAG_NONE,
            &bufferDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&constantBuffer_)),
        "Failed to create constant buffer.");

    ThrowIfFailed(constantBuffer_->Map(0, nullptr, reinterpret_cast<void**>(&mappedConstants_)), "Failed to map constant buffer.");

    D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc{};
    cbvDesc.BufferLocation = constantBuffer_->GetGPUVirtualAddress();
    cbvDesc.SizeInBytes = kConstantBufferSize;
    const D3D12_CPU_DESCRIPTOR_HANDLE heapStart = cbvSrvHeap_->GetCPUDescriptorHandleForHeapStart();
    device_->CreateConstantBufferView(&cbvDesc, heapStart);
    Log(LogLevel::Info, "Scene constant buffer created.");
    return true;
}

bool RendererDX12::CreateCheckerboardTexture()
{
    constexpr uint32_t textureWidth = 256;
    constexpr uint32_t textureHeight = 256;
    std::vector<uint8_t> pixels(textureWidth * textureHeight * 4u);
    for (uint32_t y = 0; y < textureHeight; ++y)
    {
        for (uint32_t x = 0; x < textureWidth; ++x)
        {
            const bool checker = (((x / 32) + (y / 32)) % 2) == 0;
            const uint8_t r = checker ? 245 : 60;
            const uint8_t g = checker ? 180 : 85;
            const uint8_t b = checker ? 55 : 210;
            const size_t pixelIndex = static_cast<size_t>(y * textureWidth + x) * 4u;
            pixels[pixelIndex + 0] = r;
            pixels[pixelIndex + 1] = g;
            pixels[pixelIndex + 2] = b;
            pixels[pixelIndex + 3] = 255;
        }
    }
    return CreateTextureFromRGBA(textureWidth, textureHeight, pixels, "generated checkerboard");
}

bool RendererDX12::CreateTextureFromRGBA(
    uint32_t width,
    uint32_t height,
    const std::vector<uint8_t>& rgbaPixels,
    const char* debugName)
{
    if (device_ == nullptr || commandQueue_ == nullptr)
    {
        return false;
    }

    if (texture_ != nullptr)
    {
        WaitForGpu();
        textureUpload_.Reset();
        texture_.Reset();
    }

    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC textureDesc{};
    textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureDesc.Width = width;
    textureDesc.Height = height;
    textureDesc.DepthOrArraySize = 1;
    textureDesc.MipLevels = 1;
    textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    ThrowIfFailed(
        device_->CreateCommittedResource(
            &defaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &textureDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&texture_)),
        "Failed to create texture resource.");

    UINT64 uploadBufferSize = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT numRows = 0;
    UINT64 rowSizeInBytes = 0;
    device_->GetCopyableFootprints(&textureDesc, 0, 1, 0, &footprint, &numRows, &rowSizeInBytes, &uploadBufferSize);

    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC uploadDesc{};
    uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    uploadDesc.Width = uploadBufferSize;
    uploadDesc.Height = 1;
    uploadDesc.DepthOrArraySize = 1;
    uploadDesc.MipLevels = 1;
    uploadDesc.SampleDesc.Count = 1;
    uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ThrowIfFailed(
        device_->CreateCommittedResource(
            &uploadHeap,
            D3D12_HEAP_FLAG_NONE,
            &uploadDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&textureUpload_)),
        "Failed to create texture upload buffer.");

    std::byte* uploadData = nullptr;
    ThrowIfFailed(textureUpload_->Map(0, nullptr, reinterpret_cast<void**>(&uploadData)), "Failed to map texture upload buffer.");
    for (UINT row = 0; row < numRows; ++row)
    {
        std::memcpy(
            uploadData + row * footprint.Footprint.RowPitch,
            rgbaPixels.data() + row * width * 4u,
            width * 4u);
    }
    textureUpload_->Unmap(0, nullptr);

    ThrowIfFailed(commandAllocators_[frameIndex_]->Reset(), "Failed to reset command allocator for texture upload.");
    ThrowIfFailed(commandList_->Reset(commandAllocators_[frameIndex_].Get(), nullptr), "Failed to reset command list for texture upload.");

    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = texture_.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destination.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = textureUpload_.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint = footprint;

    commandList_->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = texture_.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commandList_->ResourceBarrier(1, &barrier);

    ThrowIfFailed(commandList_->Close(), "Failed to close texture upload command list.");
    ID3D12CommandList* commandLists[] = {commandList_.Get()};
    commandQueue_->ExecuteCommandLists(1, commandLists);
    WaitForGpu();

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

    D3D12_CPU_DESCRIPTOR_HANDLE srvHandle = cbvSrvHeap_->GetCPUDescriptorHandleForHeapStart();
    srvHandle.ptr += device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    device_->CreateShaderResourceView(texture_.Get(), &srvDesc, srvHandle);

    Log(LogLevel::Info, std::string("Texture uploaded: ") + debugName);
    return true;
}

bool RendererDX12::LoadObjMesh(
    const std::wstring& path,
    std::vector<Vertex>& outVertices,
    std::vector<uint16_t>& outIndices)
{
    std::ifstream file(path);
    if (!file.is_open())
    {
        std::wstringstream stream;
        stream << L"Failed to open OBJ file: " << path;
        throw std::runtime_error(Narrow(stream.str()));
    }

    std::vector<XMFLOAT3> positions;
    std::vector<XMFLOAT3> normals;
    std::vector<XMFLOAT2> texcoords;
    std::unordered_map<std::string, uint16_t> vertexCache;

    std::string line;
    while (std::getline(file, line))
    {
        std::istringstream lineStream(line);
        std::string prefix;
        lineStream >> prefix;

        if (prefix == "v")
        {
            XMFLOAT3 position{};
            lineStream >> position.x >> position.y >> position.z;
            positions.push_back(position);
        }
        else if (prefix == "vt")
        {
            XMFLOAT2 texcoord{};
            lineStream >> texcoord.x >> texcoord.y;
            texcoord.y = 1.0f - texcoord.y;
            texcoords.push_back(texcoord);
        }
        else if (prefix == "vn")
        {
            XMFLOAT3 normal{};
            lineStream >> normal.x >> normal.y >> normal.z;
            normals.push_back(normal);
        }
        else if (prefix == "f")
        {
            std::vector<uint16_t> faceIndices;
            std::string token;
            while (lineStream >> token)
            {
                auto cached = vertexCache.find(token);
                if (cached != vertexCache.end())
                {
                    faceIndices.push_back(cached->second);
                    continue;
                }

                int positionIndex = 0;
                int texcoordIndex = 0;
                int normalIndex = 0;

                std::array<std::string, 3> parts{};
                size_t partIndex = 0;
                size_t start = 0;
                while (start <= token.size() && partIndex < parts.size())
                {
                    const size_t slash = token.find('/', start);
                    if (slash == std::string::npos)
                    {
                        parts[partIndex++] = token.substr(start);
                        break;
                    }

                    parts[partIndex++] = token.substr(start, slash - start);
                    start = slash + 1;
                    if (start == token.size())
                    {
                        parts[partIndex++] = {};
                        break;
                    }
                }

                if (!parts[0].empty())
                {
                    positionIndex = std::stoi(parts[0]);
                }
                if (!parts[1].empty())
                {
                    texcoordIndex = std::stoi(parts[1]);
                }
                if (!parts[2].empty())
                {
                    normalIndex = std::stoi(parts[2]);
                }

                if (positionIndex <= 0 || static_cast<size_t>(positionIndex) > positions.size())
                {
                    throw std::runtime_error("OBJ face references an invalid position index.");
                }

                const XMFLOAT3 position = positions[static_cast<size_t>(positionIndex - 1)];
                XMFLOAT2 texcoord{0.0f, 0.0f};
                if (texcoordIndex > 0 && static_cast<size_t>(texcoordIndex) <= texcoords.size())
                {
                    texcoord = texcoords[static_cast<size_t>(texcoordIndex - 1)];
                }

                XMFLOAT3 normal{0.0f, 1.0f, 0.0f};
                if (normalIndex > 0 && static_cast<size_t>(normalIndex) <= normals.size())
                {
                    normal = normals[static_cast<size_t>(normalIndex - 1)];
                }
                else
                {
                    const XMVECTOR normalVector = XMVector3Normalize(XMLoadFloat3(&position));
                    XMStoreFloat3(&normal, normalVector);
                }

                if (outVertices.size() >= std::numeric_limits<uint16_t>::max())
                {
                    throw std::runtime_error("OBJ mesh is too large for 16-bit indices.");
                }

                const uint16_t newIndex = static_cast<uint16_t>(outVertices.size());
                outVertices.push_back({{position.x, position.y, position.z}, {normal.x, normal.y, normal.z}, {texcoord.x, texcoord.y}});
                vertexCache.emplace(token, newIndex);
                faceIndices.push_back(newIndex);
            }

            for (size_t i = 1; i + 1 < faceIndices.size(); ++i)
            {
                outIndices.push_back(faceIndices[0]);
                outIndices.push_back(faceIndices[i]);
                outIndices.push_back(faceIndices[i + 1]);
            }
        }
    }

    if (outVertices.empty() || outIndices.empty())
    {
        throw std::runtime_error("OBJ file did not contain a valid triangle mesh.");
    }

    return true;
}

bool RendererDX12::CreateGeometryBuffers(
    const std::vector<Vertex>& primaryVertices,
    const std::vector<uint16_t>& primaryIndices,
    const char* debugName)
{
    if (primaryVertices.empty() || primaryIndices.empty())
    {
        return false;
    }

    if (vertexBuffer_ != nullptr || indexBuffer_ != nullptr)
    {
        WaitForGpu();
        vertexBuffer_.Reset();
        indexBuffer_.Reset();
    }

    std::vector<Vertex> vertices = primaryVertices;
    std::vector<uint16_t> indices = primaryIndices;

    cubeMesh_.indexCount = static_cast<uint32_t>(indices.size());
    cubeMesh_.startIndex = 0;
    cubeMesh_.baseVertex = 0;

    constexpr uint32_t sliceCount = 24;
    constexpr uint32_t stackCount = 16;
    constexpr float radius = 1.0f;
    const uint16_t sphereBaseVertex = static_cast<uint16_t>(vertices.size());

    for (uint32_t stack = 0; stack <= stackCount; ++stack)
    {
        const float v = static_cast<float>(stack) / static_cast<float>(stackCount);
        const float phi = v * XM_PI;
        for (uint32_t slice = 0; slice <= sliceCount; ++slice)
        {
            const float u = static_cast<float>(slice) / static_cast<float>(sliceCount);
            const float theta = u * XM_2PI;
            const float x = radius * std::sinf(phi) * std::cosf(theta);
            const float y = radius * std::cosf(phi);
            const float z = radius * std::sinf(phi) * std::sinf(theta);

            const XMVECTOR normalVector = XMVector3Normalize(XMVectorSet(x, y, z, 0.0f));
            XMFLOAT3 normal{};
            XMStoreFloat3(&normal, normalVector);
            vertices.push_back({{x, y, z}, {normal.x, normal.y, normal.z}, {u, v}});
        }
    }

    sphereMesh_.startIndex = static_cast<uint32_t>(indices.size());
    sphereMesh_.baseVertex = 0;
    for (uint32_t stack = 0; stack < stackCount; ++stack)
    {
        for (uint32_t slice = 0; slice < sliceCount; ++slice)
        {
            const uint16_t a = static_cast<uint16_t>(sphereBaseVertex + stack * (sliceCount + 1) + slice);
            const uint16_t b = static_cast<uint16_t>(a + sliceCount + 1);
            const uint16_t c = static_cast<uint16_t>(a + 1);
            const uint16_t d = static_cast<uint16_t>(b + 1);

            indices.push_back(a);
            indices.push_back(b);
            indices.push_back(c);
            indices.push_back(c);
            indices.push_back(b);
            indices.push_back(d);
        }
    }
    sphereMesh_.indexCount = static_cast<uint32_t>(indices.size()) - sphereMesh_.startIndex;

    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC vertexResourceDesc{};
    vertexResourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    vertexResourceDesc.Width = static_cast<UINT64>(vertices.size() * sizeof(Vertex));
    vertexResourceDesc.Height = 1;
    vertexResourceDesc.DepthOrArraySize = 1;
    vertexResourceDesc.MipLevels = 1;
    vertexResourceDesc.SampleDesc.Count = 1;
    vertexResourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ThrowIfFailed(
        device_->CreateCommittedResource(
            &uploadHeap,
            D3D12_HEAP_FLAG_NONE,
            &vertexResourceDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&vertexBuffer_)),
        "Failed to create vertex buffer.");

    void* mappedVertexData = nullptr;
    ThrowIfFailed(vertexBuffer_->Map(0, nullptr, &mappedVertexData), "Failed to map vertex buffer.");
    std::memcpy(mappedVertexData, vertices.data(), vertices.size() * sizeof(Vertex));
    vertexBuffer_->Unmap(0, nullptr);

    D3D12_RESOURCE_DESC indexResourceDesc = vertexResourceDesc;
    indexResourceDesc.Width = static_cast<UINT64>(indices.size() * sizeof(uint16_t));

    ThrowIfFailed(
        device_->CreateCommittedResource(
            &uploadHeap,
            D3D12_HEAP_FLAG_NONE,
            &indexResourceDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&indexBuffer_)),
        "Failed to create index buffer.");

    void* mappedIndexData = nullptr;
    ThrowIfFailed(indexBuffer_->Map(0, nullptr, &mappedIndexData), "Failed to map index buffer.");
    std::memcpy(mappedIndexData, indices.data(), indices.size() * sizeof(uint16_t));
    indexBuffer_->Unmap(0, nullptr);

    vertexBufferView_.BufferLocation = vertexBuffer_->GetGPUVirtualAddress();
    vertexBufferView_.StrideInBytes = sizeof(Vertex);
    vertexBufferView_.SizeInBytes = static_cast<UINT>(vertices.size() * sizeof(Vertex));
    indexBufferView_.BufferLocation = indexBuffer_->GetGPUVirtualAddress();
    indexBufferView_.Format = DXGI_FORMAT_R16_UINT;
    indexBufferView_.SizeInBytes = static_cast<UINT>(indices.size() * sizeof(uint16_t));

    Log(LogLevel::Info, std::string("Geometry updated: ") + debugName);
    return true;
}

void RendererDX12::UpdateConstants(const Scene& scene, const Camera& camera) const
{
    const float aspectRatio = static_cast<float>(std::max(width_, 1u)) / static_cast<float>(std::max(height_, 1u));
    const XMMATRIX view = XMMatrixTranspose(camera.View());
    const XMMATRIX proj = XMMatrixTranspose(camera.Projection(aspectRatio));
    const XMFLOAT3 cameraPosition = camera.Position();
    const std::array<XMMATRIX, kObjectCount> worldMatrices = {
        XMMatrixTranspose(scene.CubeWorldMatrix()),
        XMMatrixTranspose(scene.SphereWorldMatrix())};

    for (uint32_t objectIndex = 0; objectIndex < kObjectCount; ++objectIndex)
    {
        SceneConstants constants{};
        XMStoreFloat4x4(&constants.world, worldMatrices[objectIndex]);
        XMStoreFloat4x4(&constants.view, view);
        XMStoreFloat4x4(&constants.proj, proj);
        constants.cameraPos = cameraPosition;
        constants.lightDir = {0.45f, -1.0f, 0.35f};

        std::memcpy(
            reinterpret_cast<std::byte*>(mappedConstants_) + objectIndex * kPerObjectConstantBufferSize,
            &constants,
            sizeof(constants));
    }
}

void RendererDX12::PopulateCommandList(ImDrawData* imguiDrawData)
{
    ThrowIfFailed(commandAllocators_[frameIndex_]->Reset(), "Failed to reset command allocator.");
    ThrowIfFailed(commandList_->Reset(commandAllocators_[frameIndex_].Get(), pipelineState_.Get()), "Failed to reset graphics command list.");

    D3D12_RESOURCE_BARRIER beginBarrier{};
    beginBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    beginBarrier.Transition.pResource = renderTargets_[frameIndex_].Get();
    beginBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    beginBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    beginBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commandList_->ResourceBarrier(1, &beginBarrier);

    commandList_->SetGraphicsRootSignature(rootSignature_.Get());
    ID3D12DescriptorHeap* descriptorHeaps[] = {cbvSrvHeap_.Get()};
    commandList_->SetDescriptorHeaps(1, descriptorHeaps);
    commandList_->RSSetViewports(1, &viewport_);
    commandList_->RSSetScissorRects(1, &scissorRect_);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    rtvHandle.ptr += static_cast<SIZE_T>(frameIndex_) * rtvDescriptorSize_;
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = dsvHeap_->GetCPUDescriptorHandleForHeapStart();
    commandList_->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

    const float clearColor[] = {0.09f, 0.11f, 0.16f, 1.0f};
    commandList_->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
    commandList_->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    commandList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList_->IASetVertexBuffers(0, 1, &vertexBufferView_);
    commandList_->IASetIndexBuffer(&indexBufferView_);

    D3D12_GPU_DESCRIPTOR_HANDLE textureHandle = cbvSrvHeap_->GetGPUDescriptorHandleForHeapStart();
    textureHandle.ptr += device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    commandList_->SetGraphicsRootDescriptorTable(1, textureHandle);
    commandList_->SetGraphicsRootConstantBufferView(0, constantBuffer_->GetGPUVirtualAddress());
    commandList_->DrawIndexedInstanced(cubeMesh_.indexCount, 1, cubeMesh_.startIndex, cubeMesh_.baseVertex, 0);

    commandList_->SetGraphicsRootConstantBufferView(0, constantBuffer_->GetGPUVirtualAddress() + kPerObjectConstantBufferSize);
    commandList_->DrawIndexedInstanced(sphereMesh_.indexCount, 1, sphereMesh_.startIndex, sphereMesh_.baseVertex, 0);

    if (imguiDrawData != nullptr)
    {
        ID3D12DescriptorHeap* imguiDescriptorHeaps[] = {imguiSrvHeap_.Get()};
        commandList_->SetDescriptorHeaps(1, imguiDescriptorHeaps);
        ImGui_ImplDX12_RenderDrawData(imguiDrawData, commandList_.Get());
    }

    D3D12_RESOURCE_BARRIER endBarrier = beginBarrier;
    endBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    endBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    commandList_->ResourceBarrier(1, &endBarrier);

    ThrowIfFailed(commandList_->Close(), "Failed to close command list.");
}

void RendererDX12::MoveToNextFrame()
{
    const uint32_t currentFrame = frameIndex_;
    const uint64_t currentFenceValue = nextFenceValue_++;
    fenceValues_[currentFrame] = currentFenceValue;
    ThrowIfFailed(commandQueue_->Signal(fence_.Get(), currentFenceValue), "Failed to signal fence.");

    frameIndex_ = swapChain_->GetCurrentBackBufferIndex();
    if (fence_->GetCompletedValue() < fenceValues_[frameIndex_])
    {
        ThrowIfFailed(fence_->SetEventOnCompletion(fenceValues_[frameIndex_], fenceEvent_), "Failed to wait for frame fence.");
        ::WaitForSingleObject(fenceEvent_, INFINITE);
    }
}

void RendererDX12::WaitForGpu()
{
    const uint64_t fenceValue = nextFenceValue_++;
    fenceValues_[frameIndex_] = fenceValue;
    ThrowIfFailed(commandQueue_->Signal(fence_.Get(), fenceValue), "Failed to signal GPU fence.");
    ThrowIfFailed(fence_->SetEventOnCompletion(fenceValue, fenceEvent_), "Failed to set GPU fence event.");
    ::WaitForSingleObject(fenceEvent_, INFINITE);
}

void RendererDX12::RecreateRenderTargets()
{
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    for (uint32_t frame = 0; frame < kFrameCount; ++frame)
    {
        ThrowIfFailed(swapChain_->GetBuffer(frame, IID_PPV_ARGS(&renderTargets_[frame])), "Failed to get swap chain buffer.");
        device_->CreateRenderTargetView(renderTargets_[frame].Get(), nullptr, rtvHandle);
        rtvHandle.ptr += rtvDescriptorSize_;
    }
}

std::vector<std::byte> RendererDX12::LoadFileBytes(const wchar_t* path) const
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open())
    {
        std::wstringstream stream;
        stream << L"Failed to open shader bytecode file: " << path;
        throw std::runtime_error(Narrow(stream.str()));
    }

    const std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<std::byte> data(static_cast<size_t>(size));
    file.read(reinterpret_cast<char*>(data.data()), size);
    return data;
}
} // namespace engine
