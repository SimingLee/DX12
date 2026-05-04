#include "renderer/RendererDX12.h"

#include "core/Log.h"
#include "platform/Window.h"

#include "imgui.h"
#include "backends/imgui_impl_dx12.h"

#include <DirectXMath.h>
#include <d3d12sdklayers.h>
#include <wincodec.h>

#if ENGINE_HAS_ASSIMP
#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#endif

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <cmath>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <limits>
#include <optional>
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
constexpr DXGI_FORMAT kShadowMapResourceFormat = DXGI_FORMAT_R32_TYPELESS;
constexpr DXGI_FORMAT kShadowMapDsvFormat = DXGI_FORMAT_D32_FLOAT;
constexpr DXGI_FORMAT kShadowMapSrvFormat = DXGI_FORMAT_R32_FLOAT;
constexpr DXGI_FORMAT kGBufferAlbedoFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr DXGI_FORMAT kGBufferNormalFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr DXGI_FORMAT kGBufferWorldPositionFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr uint32_t kDiffuseSrvIndex = 0;
constexpr uint32_t kGBufferSrvStartIndex = 1;
constexpr uint32_t kDirectionalShadowSrvIndex = kGBufferSrvStartIndex + 3;
constexpr uint32_t kPointShadowSrvIndex = kDirectionalShadowSrvIndex + 1;
constexpr uint32_t kLightTypeDirectional = 0;
constexpr uint32_t kLightTypePoint = 1;
constexpr uint32_t kShadowTypeNone = 0;
constexpr uint32_t kShadowTypeDirectional = 1;
constexpr uint32_t kShadowTypePoint = 2;
constexpr float kPointShadowNearPlane = 0.1f;

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

std::wstring LowerExtension(const std::wstring& path)
{
    std::wstring extension = std::filesystem::path(path).extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(std::towlower(c));
    });
    return extension;
}

std::string FileStemUtf8(const std::wstring& path)
{
    const std::wstring stem = std::filesystem::path(path).stem().wstring();
    return stem.empty() ? "Imported Mesh" : Narrow(stem);
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
        if (!CreateRootSignatureAndPipelines())
        {
            return false;
        }
        if (!CreateGeometry())
        {
            return false;
        }
        if (!CreateConstantBuffers())
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
    return LoadMeshAssetFromFile(path) != kInvalidMeshAssetId;
}

uint32_t RendererDX12::LoadMeshAssetFromFile(const std::wstring& path)
{
    try
    {
        const std::filesystem::path normalizedPath = std::filesystem::absolute(std::filesystem::path(path));
        const std::wstring normalized = normalizedPath.wstring();
        for (const MeshAsset& asset : meshAssets_)
        {
            if (!asset.info.sourcePath.empty() && asset.info.sourcePath == normalized)
            {
                loadedModelPath_ = normalized;
                return asset.info.id;
            }
        }

        std::vector<Vertex> vertices;
        std::vector<uint16_t> indices;
        if (!LoadImportedMesh(normalized, vertices, indices))
        {
            return kInvalidMeshAssetId;
        }

        if (meshVertices_.size() + vertices.size() > std::numeric_limits<uint16_t>::max())
        {
            Log(LogLevel::Error, "Imported mesh set is too large for the current 16-bit index buffer.");
            return kInvalidMeshAssetId;
        }

        const uint32_t baseVertex = static_cast<uint32_t>(meshVertices_.size());
        const uint32_t startIndex = static_cast<uint32_t>(meshIndices_.size());
        for (uint16_t index : indices)
        {
            meshIndices_.push_back(static_cast<uint16_t>(baseVertex + index));
        }
        meshVertices_.insert(meshVertices_.end(), vertices.begin(), vertices.end());

        MeshAsset asset{};
        asset.info.id = nextMeshAssetId_++;
        asset.info.sourcePath = normalized;
        asset.info.name = FileStemUtf8(normalized);
        asset.range.indexCount = static_cast<uint32_t>(indices.size());
        asset.range.startIndex = startIndex;
        asset.range.baseVertex = 0;
        meshAssets_.push_back(asset);

        if (!CreateGeometryBuffers("imported mesh asset"))
        {
            meshAssets_.pop_back();
            meshVertices_.resize(baseVertex);
            meshIndices_.resize(startIndex);
            return kInvalidMeshAssetId;
        }

        loadedModelPath_ = normalized;
        Log(LogLevel::Info, std::string("Loaded mesh asset: ") + asset.info.name);
        return asset.info.id;
    }
    catch (const std::exception& exception)
    {
        Log(LogLevel::Error, exception.what());
        return kInvalidMeshAssetId;
    }
}

const MeshAssetInfo* RendererDX12::FindMeshAsset(uint32_t id) const
{
    const MeshAsset* asset = FindMeshAssetInternal(id);
    return asset != nullptr ? &asset->info : nullptr;
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
    for (auto& gbufferTarget : gbufferTargets_)
    {
        gbufferTarget.Reset();
    }
    depthStencil_.Reset();
    directionalShadowMap_.Reset();
    pointShadowMap_.Reset();

    DXGI_SWAP_CHAIN_DESC swapChainDesc{};
    ThrowIfFailed(swapChain_->GetDesc(&swapChainDesc), "Failed to read swap chain description.");
    ThrowIfFailed(
        swapChain_->ResizeBuffers(kFrameCount, width_, height_, swapChainDesc.BufferDesc.Format, swapChainDesc.Flags),
        "Failed to resize swap chain buffers.");

    frameIndex_ = swapChain_->GetCurrentBackBufferIndex();
    RecreateRenderTargets();
    ThrowIfFailed(CreateDepthStencil(width_, height_) ? S_OK : E_FAIL, "Failed to recreate depth stencil.");
    ThrowIfFailed(CreateGBufferResources(width_, height_) ? S_OK : E_FAIL, "Failed to recreate G-buffer.");
    ThrowIfFailed(CreateShadowResources() ? S_OK : E_FAIL, "Failed to recreate shadow resources.");
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
    UpdateLightingConstants(scene);
    PopulateCommandList(scene, imguiDrawData);

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

    mappedObjectConstants_ = nullptr;
    mappedShadowObjectConstants_ = nullptr;
    mappedLightingConstants_ = nullptr;
    textureUpload_.Reset();
    texture_.Reset();
    for (auto& gbufferTarget : gbufferTargets_)
    {
        gbufferTarget.Reset();
    }
    depthStencil_.Reset();
    directionalShadowMap_.Reset();
    pointShadowMap_.Reset();
    lightingConstantBuffer_.Reset();
    shadowObjectConstantBuffer_.Reset();
    objectConstantBuffer_.Reset();
    indexBuffer_.Reset();
    vertexBuffer_.Reset();
    deferredLightingPipelineState_.Reset();
    gbufferPipelineState_.Reset();
    forwardPipelineState_.Reset();
    shadowPipelineState_.Reset();
    pointShadowPipelineState_.Reset();
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
    rtvHeapDesc.NumDescriptors = kFrameCount + kGBufferTargetCount;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    ThrowIfFailed(device_->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&rtvHeap_)), "Failed to create RTV heap.");

    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc{};
    dsvHeapDesc.NumDescriptors = 1 + kMaxLightCount + kMaxLightCount * kPointShadowFaceCount;
    dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    ThrowIfFailed(device_->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&dsvHeap_)), "Failed to create DSV heap.");

    D3D12_DESCRIPTOR_HEAP_DESC cbvSrvHeapDesc{};
    cbvSrvHeapDesc.NumDescriptors = 1 + kGBufferTargetCount + 2;
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
    dsvDescriptorSize_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

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
    return CreateDepthStencil(width_, height_) && CreateGBufferResources(width_, height_) && CreateShadowResources();
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

bool RendererDX12::CreateGBufferResources(uint32_t width, uint32_t height)
{
    const std::array<DXGI_FORMAT, kGBufferTargetCount> formats = {
        kGBufferAlbedoFormat,
        kGBufferNormalFormat,
        kGBufferWorldPositionFormat};
    const std::array<const wchar_t*, kGBufferTargetCount> names = {
        L"GBuffer Albedo",
        L"GBuffer Normal",
        L"GBuffer World Position"};

    D3D12_HEAP_PROPERTIES heapProperties{};
    heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    rtvHandle.ptr += static_cast<SIZE_T>(kFrameCount) * rtvDescriptorSize_;

    D3D12_CPU_DESCRIPTOR_HANDLE srvHandle = cbvSrvHeap_->GetCPUDescriptorHandleForHeapStart();
    srvHandle.ptr += static_cast<SIZE_T>(kGBufferSrvStartIndex) * cbvSrvDescriptorSize_;

    for (uint32_t target = 0; target < kGBufferTargetCount; ++target)
    {
        D3D12_RESOURCE_DESC resourceDesc{};
        resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        resourceDesc.Width = width;
        resourceDesc.Height = height;
        resourceDesc.DepthOrArraySize = 1;
        resourceDesc.MipLevels = 1;
        resourceDesc.Format = formats[target];
        resourceDesc.SampleDesc.Count = 1;
        resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        resourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        D3D12_CLEAR_VALUE clearValue{};
        clearValue.Format = formats[target];
        clearValue.Color[0] = 0.0f;
        clearValue.Color[1] = 0.0f;
        clearValue.Color[2] = 0.0f;
        clearValue.Color[3] = 1.0f;

        ThrowIfFailed(
            device_->CreateCommittedResource(
                &heapProperties,
                D3D12_HEAP_FLAG_NONE,
                &resourceDesc,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                &clearValue,
                IID_PPV_ARGS(&gbufferTargets_[target])),
            "Failed to create G-buffer target.");
        gbufferTargets_[target]->SetName(names[target]);

        D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
        rtvDesc.Format = formats[target];
        rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        device_->CreateRenderTargetView(gbufferTargets_[target].Get(), &rtvDesc, rtvHandle);

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = formats[target];
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(gbufferTargets_[target].Get(), &srvDesc, srvHandle);

        rtvHandle.ptr += rtvDescriptorSize_;
        srvHandle.ptr += cbvSrvDescriptorSize_;
    }

    return true;
}

bool RendererDX12::CreateShadowResources()
{
    D3D12_HEAP_PROPERTIES heapProperties{};
    heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_CLEAR_VALUE clearValue{};
    clearValue.Format = kShadowMapDsvFormat;
    clearValue.DepthStencil.Depth = 1.0f;

    D3D12_RESOURCE_DESC directionalDesc{};
    directionalDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    directionalDesc.Width = kShadowMapSize;
    directionalDesc.Height = kShadowMapSize;
    directionalDesc.DepthOrArraySize = kMaxLightCount;
    directionalDesc.MipLevels = 1;
    directionalDesc.Format = kShadowMapResourceFormat;
    directionalDesc.SampleDesc.Count = 1;
    directionalDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    directionalDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    ThrowIfFailed(
        device_->CreateCommittedResource(
            &heapProperties,
            D3D12_HEAP_FLAG_NONE,
            &directionalDesc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            &clearValue,
            IID_PPV_ARGS(&directionalShadowMap_)),
        "Failed to create directional shadow map.");
    directionalShadowMap_->SetName(L"Directional Shadow Map Array");

    D3D12_RESOURCE_DESC pointDesc = directionalDesc;
    pointDesc.Width = kPointShadowMapSize;
    pointDesc.Height = kPointShadowMapSize;
    pointDesc.DepthOrArraySize = kMaxLightCount * kPointShadowFaceCount;
    ThrowIfFailed(
        device_->CreateCommittedResource(
            &heapProperties,
            D3D12_HEAP_FLAG_NONE,
            &pointDesc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            &clearValue,
            IID_PPV_ARGS(&pointShadowMap_)),
        "Failed to create point shadow cubemap array.");
    pointShadowMap_->SetName(L"Point Shadow Cubemap Array");

    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = dsvHeap_->GetCPUDescriptorHandleForHeapStart();
    dsvHandle.ptr += dsvDescriptorSize_;
    for (uint32_t slice = 0; slice < kMaxLightCount; ++slice)
    {
        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
        dsvDesc.Format = kShadowMapDsvFormat;
        dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        dsvDesc.Texture2DArray.MipSlice = 0;
        dsvDesc.Texture2DArray.FirstArraySlice = slice;
        dsvDesc.Texture2DArray.ArraySize = 1;
        device_->CreateDepthStencilView(directionalShadowMap_.Get(), &dsvDesc, dsvHandle);
        dsvHandle.ptr += dsvDescriptorSize_;
    }

    for (uint32_t slice = 0; slice < kMaxLightCount * kPointShadowFaceCount; ++slice)
    {
        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
        dsvDesc.Format = kShadowMapDsvFormat;
        dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        dsvDesc.Texture2DArray.MipSlice = 0;
        dsvDesc.Texture2DArray.FirstArraySlice = slice;
        dsvDesc.Texture2DArray.ArraySize = 1;
        device_->CreateDepthStencilView(pointShadowMap_.Get(), &dsvDesc, dsvHandle);
        dsvHandle.ptr += dsvDescriptorSize_;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE srvHandle = cbvSrvHeap_->GetCPUDescriptorHandleForHeapStart();
    srvHandle.ptr += static_cast<SIZE_T>(kDirectionalShadowSrvIndex) * cbvSrvDescriptorSize_;

    D3D12_SHADER_RESOURCE_VIEW_DESC directionalSrvDesc{};
    directionalSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    directionalSrvDesc.Format = kShadowMapSrvFormat;
    directionalSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    directionalSrvDesc.Texture2DArray.MipLevels = 1;
    directionalSrvDesc.Texture2DArray.ArraySize = kMaxLightCount;
    device_->CreateShaderResourceView(directionalShadowMap_.Get(), &directionalSrvDesc, srvHandle);

    srvHandle.ptr += cbvSrvDescriptorSize_;
    D3D12_SHADER_RESOURCE_VIEW_DESC pointSrvDesc{};
    pointSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    pointSrvDesc.Format = kShadowMapSrvFormat;
    pointSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
    pointSrvDesc.TextureCubeArray.MipLevels = 1;
    pointSrvDesc.TextureCubeArray.NumCubes = kMaxLightCount;
    device_->CreateShaderResourceView(pointShadowMap_.Get(), &pointSrvDesc, srvHandle);

    shadowViewport_ = {0.0f, 0.0f, static_cast<float>(kShadowMapSize), static_cast<float>(kShadowMapSize), 0.0f, 1.0f};
    shadowScissorRect_ = {0, 0, static_cast<LONG>(kShadowMapSize), static_cast<LONG>(kShadowMapSize)};
    pointShadowViewport_ = {0.0f, 0.0f, static_cast<float>(kPointShadowMapSize), static_cast<float>(kPointShadowMapSize), 0.0f, 1.0f};
    pointShadowScissorRect_ = {0, 0, static_cast<LONG>(kPointShadowMapSize), static_cast<LONG>(kPointShadowMapSize)};
    return true;
}

bool RendererDX12::CreateRootSignatureAndPipelines()
{
    std::array<D3D12_DESCRIPTOR_RANGE1, 2> descriptorRanges{};
    descriptorRanges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    descriptorRanges[0].NumDescriptors = 3;
    descriptorRanges[0].BaseShaderRegister = 0;
    descriptorRanges[0].RegisterSpace = 0;
    descriptorRanges[0].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
    descriptorRanges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    descriptorRanges[1].NumDescriptors = 2;
    descriptorRanges[1].BaseShaderRegister = 3;
    descriptorRanges[1].RegisterSpace = 0;
    descriptorRanges[1].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;

    std::array<D3D12_ROOT_PARAMETER1, 4> rootParameters{};
    rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParameters[0].Descriptor.ShaderRegister = 0;
    rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rootParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParameters[1].Descriptor.ShaderRegister = 1;
    rootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rootParameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[2].DescriptorTable.NumDescriptorRanges = 1;
    rootParameters[2].DescriptorTable.pDescriptorRanges = &descriptorRanges[0];
    rootParameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rootParameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[3].DescriptorTable.NumDescriptorRanges = 1;
    rootParameters[3].DescriptorTable.pDescriptorRanges = &descriptorRanges[1];
    rootParameters[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    std::array<D3D12_STATIC_SAMPLER_DESC, 2> samplers{};
    samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[0].ShaderRegister = 0;
    samplers[0].RegisterSpace = 0;
    samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    samplers[0].MaxLOD = D3D12_FLOAT32_MAX;
    samplers[1].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    samplers[1].AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    samplers[1].AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    samplers[1].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    samplers[1].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    samplers[1].ShaderRegister = 1;
    samplers[1].RegisterSpace = 0;
    samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    samplers[1].MaxLOD = D3D12_FLOAT32_MAX;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC rootSignatureDesc{};
    rootSignatureDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    rootSignatureDesc.Desc_1_1.NumParameters = static_cast<UINT>(rootParameters.size());
    rootSignatureDesc.Desc_1_1.pParameters = rootParameters.data();
    rootSignatureDesc.Desc_1_1.NumStaticSamplers = static_cast<UINT>(samplers.size());
    rootSignatureDesc.Desc_1_1.pStaticSamplers = samplers.data();
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

    return CreateForwardPipeline() && CreateDeferredPipelines() && CreateShadowPipeline();
}

bool RendererDX12::CreateForwardPipeline()
{
    const std::wstring shaderDir = L"" ENGINE_SHADER_DIR;
    const auto vertexShader = LoadFileBytes((shaderDir + L"/forward_vs.cso").c_str());
    const auto pixelShader = LoadFileBytes((shaderDir + L"/forward_ps.cso").c_str());

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

    ThrowIfFailed(device_->CreateGraphicsPipelineState(&pipelineDesc, IID_PPV_ARGS(&forwardPipelineState_)), "Failed to create forward graphics pipeline state.");
    Log(LogLevel::Info, "Forward graphics pipeline created.");
    return true;
}

bool RendererDX12::CreateDeferredPipelines()
{
    const std::wstring shaderDir = L"" ENGINE_SHADER_DIR;
    const auto gbufferVertexShader = LoadFileBytes((shaderDir + L"/gbuffer_vs.cso").c_str());
    const auto gbufferPixelShader = LoadFileBytes((shaderDir + L"/gbuffer_ps.cso").c_str());
    const auto lightingVertexShader = LoadFileBytes((shaderDir + L"/deferred_lighting_vs.cso").c_str());
    const auto lightingPixelShader = LoadFileBytes((shaderDir + L"/deferred_lighting_ps.cso").c_str());

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
    for (auto& renderTargetBlendDesc : blendDesc.RenderTarget)
    {
        renderTargetBlendDesc = defaultRenderTargetBlendDesc;
    }

    D3D12_DEPTH_STENCIL_DESC depthStencilDesc{};
    depthStencilDesc.DepthEnable = TRUE;
    depthStencilDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    depthStencilDesc.DepthFunc = D3D12_COMPARISON_FUNC_LESS;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC gbufferPipelineDesc{};
    gbufferPipelineDesc.pRootSignature = rootSignature_.Get();
    gbufferPipelineDesc.VS = {gbufferVertexShader.data(), gbufferVertexShader.size()};
    gbufferPipelineDesc.PS = {gbufferPixelShader.data(), gbufferPixelShader.size()};
    gbufferPipelineDesc.BlendState = blendDesc;
    gbufferPipelineDesc.SampleMask = UINT_MAX;
    gbufferPipelineDesc.RasterizerState = rasterizerDesc;
    gbufferPipelineDesc.DepthStencilState = depthStencilDesc;
    gbufferPipelineDesc.InputLayout = {inputLayout, static_cast<UINT>(std::size(inputLayout))};
    gbufferPipelineDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    gbufferPipelineDesc.NumRenderTargets = kGBufferTargetCount;
    gbufferPipelineDesc.RTVFormats[0] = kGBufferAlbedoFormat;
    gbufferPipelineDesc.RTVFormats[1] = kGBufferNormalFormat;
    gbufferPipelineDesc.RTVFormats[2] = kGBufferWorldPositionFormat;
    gbufferPipelineDesc.DSVFormat = kDepthFormat;
    gbufferPipelineDesc.SampleDesc.Count = 1;

    ThrowIfFailed(device_->CreateGraphicsPipelineState(&gbufferPipelineDesc, IID_PPV_ARGS(&gbufferPipelineState_)), "Failed to create G-buffer pipeline state.");

    D3D12_RASTERIZER_DESC fullscreenRasterizerDesc = rasterizerDesc;
    fullscreenRasterizerDesc.CullMode = D3D12_CULL_MODE_NONE;

    D3D12_DEPTH_STENCIL_DESC noDepthStencilDesc{};
    noDepthStencilDesc.DepthEnable = FALSE;
    noDepthStencilDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    noDepthStencilDesc.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC lightingPipelineDesc{};
    lightingPipelineDesc.pRootSignature = rootSignature_.Get();
    lightingPipelineDesc.VS = {lightingVertexShader.data(), lightingVertexShader.size()};
    lightingPipelineDesc.PS = {lightingPixelShader.data(), lightingPixelShader.size()};
    lightingPipelineDesc.BlendState = blendDesc;
    lightingPipelineDesc.SampleMask = UINT_MAX;
    lightingPipelineDesc.RasterizerState = fullscreenRasterizerDesc;
    lightingPipelineDesc.DepthStencilState = noDepthStencilDesc;
    lightingPipelineDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    lightingPipelineDesc.NumRenderTargets = 1;
    lightingPipelineDesc.RTVFormats[0] = kBackBufferFormat;
    lightingPipelineDesc.SampleDesc.Count = 1;

    ThrowIfFailed(device_->CreateGraphicsPipelineState(&lightingPipelineDesc, IID_PPV_ARGS(&deferredLightingPipelineState_)), "Failed to create deferred lighting pipeline state.");
    Log(LogLevel::Info, "Deferred G-buffer and lighting pipelines created.");
    return true;
}

bool RendererDX12::CreateShadowPipeline()
{
    const std::wstring shaderDir = L"" ENGINE_SHADER_DIR;
    const auto shadowVertexShader = LoadFileBytes((shaderDir + L"/shadow_vs.cso").c_str());
    const auto pointShadowVertexShader = LoadFileBytes((shaderDir + L"/point_shadow_vs.cso").c_str());

    const D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_RASTERIZER_DESC rasterizerDesc{};
    rasterizerDesc.FillMode = D3D12_FILL_MODE_SOLID;
    rasterizerDesc.CullMode = D3D12_CULL_MODE_BACK;
    rasterizerDesc.FrontCounterClockwise = FALSE;
    rasterizerDesc.DepthBias = 1500;
    rasterizerDesc.SlopeScaledDepthBias = 1.5f;
    rasterizerDesc.DepthClipEnable = TRUE;

    D3D12_BLEND_DESC blendDesc{};
    blendDesc.AlphaToCoverageEnable = FALSE;
    blendDesc.IndependentBlendEnable = FALSE;

    D3D12_DEPTH_STENCIL_DESC depthStencilDesc{};
    depthStencilDesc.DepthEnable = TRUE;
    depthStencilDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    depthStencilDesc.DepthFunc = D3D12_COMPARISON_FUNC_LESS;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipelineDesc{};
    pipelineDesc.pRootSignature = rootSignature_.Get();
    pipelineDesc.VS = {shadowVertexShader.data(), shadowVertexShader.size()};
    pipelineDesc.BlendState = blendDesc;
    pipelineDesc.SampleMask = UINT_MAX;
    pipelineDesc.RasterizerState = rasterizerDesc;
    pipelineDesc.DepthStencilState = depthStencilDesc;
    pipelineDesc.InputLayout = {inputLayout, static_cast<UINT>(std::size(inputLayout))};
    pipelineDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pipelineDesc.NumRenderTargets = 0;
    pipelineDesc.DSVFormat = kShadowMapDsvFormat;
    pipelineDesc.SampleDesc.Count = 1;

    ThrowIfFailed(device_->CreateGraphicsPipelineState(&pipelineDesc, IID_PPV_ARGS(&shadowPipelineState_)), "Failed to create shadow pipeline state.");
    pipelineDesc.VS = {pointShadowVertexShader.data(), pointShadowVertexShader.size()};
    ThrowIfFailed(device_->CreateGraphicsPipelineState(&pipelineDesc, IID_PPV_ARGS(&pointShadowPipelineState_)), "Failed to create point shadow pipeline state.");
    Log(LogLevel::Info, "Shadow map pipeline created.");
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
    meshVertices_.clear();
    meshIndices_.clear();
    meshAssets_.clear();
    nextMeshAssetId_ = kSphereMeshAssetId + 1;

    const std::vector<Vertex> cubeVertices = {{
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

    const std::vector<uint16_t> cubeIndices = {
        0, 2, 1, 0, 3, 2,
        4, 6, 5, 4, 7, 6,
        8, 10, 9, 8, 11, 10,
        12, 14, 13, 12, 15, 14,
        16, 18, 17, 16, 19, 18,
        20, 22, 21, 20, 23, 22};

    cubeMesh_.indexCount = static_cast<uint32_t>(cubeIndices.size());
    cubeMesh_.startIndex = 0;
    cubeMesh_.baseVertex = 0;
    meshVertices_ = cubeVertices;
    meshIndices_ = cubeIndices;
    meshAssets_.push_back(MeshAsset{MeshAssetInfo{kCubeMeshAssetId, {}, "Built-in Cube", false}, cubeMesh_});

    constexpr uint32_t sliceCount = 24;
    constexpr uint32_t stackCount = 16;
    constexpr float radius = 1.0f;
    const uint16_t sphereBaseVertex = static_cast<uint16_t>(meshVertices_.size());

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
            meshVertices_.push_back({{x, y, z}, {normal.x, normal.y, normal.z}, {u, v}});
        }
    }

    sphereMesh_.startIndex = static_cast<uint32_t>(meshIndices_.size());
    sphereMesh_.baseVertex = 0;
    for (uint32_t stack = 0; stack < stackCount; ++stack)
    {
        for (uint32_t slice = 0; slice < sliceCount; ++slice)
        {
            const uint16_t a = static_cast<uint16_t>(sphereBaseVertex + stack * (sliceCount + 1) + slice);
            const uint16_t b = static_cast<uint16_t>(a + sliceCount + 1);
            const uint16_t c = static_cast<uint16_t>(a + 1);
            const uint16_t d = static_cast<uint16_t>(b + 1);

            meshIndices_.push_back(c);
            meshIndices_.push_back(b);
            meshIndices_.push_back(a);
            meshIndices_.push_back(c);
            meshIndices_.push_back(d);
            meshIndices_.push_back(b);
        }
    }
    sphereMesh_.indexCount = static_cast<uint32_t>(meshIndices_.size()) - sphereMesh_.startIndex;
    meshAssets_.push_back(MeshAsset{MeshAssetInfo{kSphereMeshAssetId, {}, "Built-in Sphere", false}, sphereMesh_});

    return CreateGeometryBuffers("built-in meshes");
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

bool RendererDX12::CreateConstantBuffers()
{
    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC bufferDesc{};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = kObjectConstantBufferSize;
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
            IID_PPV_ARGS(&objectConstantBuffer_)),
        "Failed to create object constant buffer.");

    ThrowIfFailed(objectConstantBuffer_->Map(0, nullptr, reinterpret_cast<void**>(&mappedObjectConstants_)), "Failed to map object constant buffer.");

    bufferDesc.Width = kShadowObjectConstantBufferSize;
    ThrowIfFailed(
        device_->CreateCommittedResource(
            &uploadHeap,
            D3D12_HEAP_FLAG_NONE,
            &bufferDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&shadowObjectConstantBuffer_)),
        "Failed to create shadow object constant buffer.");

    ThrowIfFailed(shadowObjectConstantBuffer_->Map(0, nullptr, reinterpret_cast<void**>(&mappedShadowObjectConstants_)), "Failed to map shadow object constant buffer.");

    bufferDesc.Width = kLightingConstantBufferSize;
    ThrowIfFailed(
        device_->CreateCommittedResource(
            &uploadHeap,
            D3D12_HEAP_FLAG_NONE,
            &bufferDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&lightingConstantBuffer_)),
        "Failed to create lighting constant buffer.");

    ThrowIfFailed(lightingConstantBuffer_->Map(0, nullptr, reinterpret_cast<void**>(&mappedLightingConstants_)), "Failed to map lighting constant buffer.");

    Log(LogLevel::Info, "Object and lighting constant buffers created.");
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
    srvHandle.ptr += static_cast<SIZE_T>(kDiffuseSrvIndex) * cbvSrvDescriptorSize_;
    device_->CreateShaderResourceView(texture_.Get(), &srvDesc, srvHandle);

    Log(LogLevel::Info, std::string("Texture uploaded: ") + debugName);
    return true;
}

bool RendererDX12::LoadObjMesh(
    const std::wstring& path,
    std::vector<Vertex>& outVertices,
    std::vector<uint16_t>& outIndices)
{
    std::ifstream file{std::filesystem::path(path)};
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

bool RendererDX12::LoadImportedMesh(
    const std::wstring& path,
    std::vector<Vertex>& outVertices,
    std::vector<uint16_t>& outIndices)
{
    const std::wstring extension = LowerExtension(path);
    if (extension == L".obj")
    {
        return LoadObjMesh(path, outVertices, outIndices);
    }

#if ENGINE_HAS_ASSIMP
    Assimp::Importer importer;
    const unsigned int flags =
        aiProcess_Triangulate |
        aiProcess_GenSmoothNormals |
        aiProcess_JoinIdenticalVertices |
        aiProcess_ImproveCacheLocality |
        aiProcess_FlipUVs;
    const aiScene* scene = importer.ReadFile(Narrow(path), flags);
    if (scene == nullptr || scene->mRootNode == nullptr || scene->mNumMeshes == 0)
    {
        throw std::runtime_error(std::string("Assimp failed to import mesh: ") + importer.GetErrorString());
    }

    for (uint32_t meshIndex = 0; meshIndex < scene->mNumMeshes; ++meshIndex)
    {
        const aiMesh* mesh = scene->mMeshes[meshIndex];
        if (mesh == nullptr || mesh->mNumVertices == 0 || mesh->mNumFaces == 0)
        {
            continue;
        }

        if (outVertices.size() + mesh->mNumVertices > std::numeric_limits<uint16_t>::max())
        {
            throw std::runtime_error("Imported mesh is too large for 16-bit indices.");
        }

        const uint16_t baseVertex = static_cast<uint16_t>(outVertices.size());
        for (uint32_t vertexIndex = 0; vertexIndex < mesh->mNumVertices; ++vertexIndex)
        {
            const aiVector3D position = mesh->mVertices[vertexIndex];
            const aiVector3D normal = mesh->HasNormals() ? mesh->mNormals[vertexIndex] : aiVector3D(0.0f, 1.0f, 0.0f);
            const aiVector3D uv = mesh->HasTextureCoords(0) ? mesh->mTextureCoords[0][vertexIndex] : aiVector3D(0.0f, 0.0f, 0.0f);
            outVertices.push_back({
                {position.x, position.y, position.z},
                {normal.x, normal.y, normal.z},
                {uv.x, uv.y}});
        }

        for (uint32_t faceIndex = 0; faceIndex < mesh->mNumFaces; ++faceIndex)
        {
            const aiFace& face = mesh->mFaces[faceIndex];
            if (face.mNumIndices != 3)
            {
                continue;
            }
            outIndices.push_back(static_cast<uint16_t>(baseVertex + face.mIndices[0]));
            outIndices.push_back(static_cast<uint16_t>(baseVertex + face.mIndices[1]));
            outIndices.push_back(static_cast<uint16_t>(baseVertex + face.mIndices[2]));
        }
    }

    if (outVertices.empty() || outIndices.empty())
    {
        throw std::runtime_error("Imported file did not contain a valid static triangle mesh.");
    }
    return true;
#else
    std::wstringstream stream;
    stream << L"Assimp is not available, so only OBJ import is enabled in this build. File: " << path;
    Log(LogLevel::Error, Narrow(stream.str()));
    return false;
#endif
}

bool RendererDX12::CreateGeometryBuffers(const char* debugName)
{
    if (meshVertices_.empty() || meshIndices_.empty())
    {
        return false;
    }

    if (vertexBuffer_ != nullptr || indexBuffer_ != nullptr)
    {
        WaitForGpu();
        vertexBuffer_.Reset();
        indexBuffer_.Reset();
    }

    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC vertexResourceDesc{};
    vertexResourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    vertexResourceDesc.Width = static_cast<UINT64>(meshVertices_.size() * sizeof(Vertex));
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
    std::memcpy(mappedVertexData, meshVertices_.data(), meshVertices_.size() * sizeof(Vertex));
    vertexBuffer_->Unmap(0, nullptr);

    D3D12_RESOURCE_DESC indexResourceDesc = vertexResourceDesc;
    indexResourceDesc.Width = static_cast<UINT64>(meshIndices_.size() * sizeof(uint16_t));

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
    std::memcpy(mappedIndexData, meshIndices_.data(), meshIndices_.size() * sizeof(uint16_t));
    indexBuffer_->Unmap(0, nullptr);

    vertexBufferView_.BufferLocation = vertexBuffer_->GetGPUVirtualAddress();
    vertexBufferView_.StrideInBytes = sizeof(Vertex);
    vertexBufferView_.SizeInBytes = static_cast<UINT>(meshVertices_.size() * sizeof(Vertex));
    indexBufferView_.BufferLocation = indexBuffer_->GetGPUVirtualAddress();
    indexBufferView_.Format = DXGI_FORMAT_R16_UINT;
    indexBufferView_.SizeInBytes = static_cast<UINT>(meshIndices_.size() * sizeof(uint16_t));

    Log(LogLevel::Info, std::string("Geometry updated: ") + debugName);
    return true;
}

const RendererDX12::MeshRange* RendererDX12::FindMeshRange(uint32_t meshAssetId) const
{
    if (const MeshAsset* asset = FindMeshAssetInternal(meshAssetId))
    {
        return &asset->range;
    }
    return &cubeMesh_;
}

const RendererDX12::MeshAsset* RendererDX12::FindMeshAssetInternal(uint32_t id) const
{
    const auto assetIt = std::find_if(meshAssets_.begin(), meshAssets_.end(), [id](const MeshAsset& asset) {
        return asset.info.id == id;
    });
    return assetIt != meshAssets_.end() ? &(*assetIt) : nullptr;
}

void RendererDX12::UpdateConstants(const Scene& scene, const Camera& camera) const
{
    const float aspectRatio = static_cast<float>(std::max(width_, 1u)) / static_cast<float>(std::max(height_, 1u));
    const XMMATRIX view = XMMatrixTranspose(camera.View());
    const XMMATRIX proj = XMMatrixTranspose(camera.Projection(aspectRatio));
    const XMFLOAT3 cameraPosition = camera.Position();
    const std::vector<SceneObject>& objects = scene.Objects();
    const uint32_t objectCount = std::min<uint32_t>(static_cast<uint32_t>(objects.size()), kMaxObjectCount);
    const std::optional<uint32_t> selectedObjectId = scene.SelectedObjectId();

    for (uint32_t objectIndex = 0; objectIndex < objectCount; ++objectIndex)
    {
        const SceneObject& object = objects[objectIndex];
        ObjectConstants constants{};
        XMStoreFloat4x4(&constants.world, XMMatrixTranspose(object.transform.WorldMatrix()));
        XMStoreFloat4x4(&constants.view, view);
        XMStoreFloat4x4(&constants.proj, proj);
        constants.cameraPos = cameraPosition;
        constants.tint = selectedObjectId == object.id ?
            XMFLOAT4{1.0f, 0.78f, 0.28f, 0.35f} :
            XMFLOAT4{1.0f, 1.0f, 1.0f, 0.0f};

        std::memcpy(
            reinterpret_cast<std::byte*>(mappedObjectConstants_) + objectIndex * kPerObjectConstantBufferSize,
            &constants,
            sizeof(constants));
    }
}

void RendererDX12::UpdateLightingConstants(const Scene& scene) const
{
    LightingConstants constants{};
    constants.ambientColor = {0.0f, 0.0f, 0.0f, 1.0f};
    constants.lightCount = 0;
    uint32_t directionalShadowSlot = 0;
    uint32_t pointShadowSlot = 0;

    for (const SceneObject& object : scene.Objects())
    {
        if (!object.light.has_value() || !object.light->enabled || constants.lightCount >= kMaxLightCount)
        {
            continue;
        }

        GpuLight& gpuLight = constants.lights[constants.lightCount++];
        gpuLight.positionRange = {
            object.transform.position.x,
            object.transform.position.y,
            object.transform.position.z,
            object.light->range};

        const XMMATRIX rotation = XMMatrixRotationRollPitchYaw(
            object.transform.rotationEuler.x,
            object.transform.rotationEuler.y,
            object.transform.rotationEuler.z);
        const XMVECTOR direction = XMVector3Normalize(XMVector3TransformNormal(XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f), rotation));
        const float type = object.light->type == LightType::Point ?
            static_cast<float>(kLightTypePoint) :
            static_cast<float>(kLightTypeDirectional);
        XMStoreFloat4(&gpuLight.directionType, XMVectorSetW(direction, type));
        gpuLight.colorIntensity = {
            object.light->color.x,
            object.light->color.y,
            object.light->color.z,
            object.light->intensity};

        gpuLight.shadowData = {
            static_cast<float>(kShadowTypeNone),
            0.0f,
            0.0f,
            object.light->shadow.bias};
        if (object.light->shadow.enabled)
        {
            if (object.light->type == LightType::Directional && directionalShadowSlot < kMaxLightCount)
            {
                XMMATRIX lightView{};
                XMMATRIX lightProjection{};
                if (BuildDirectionalShadowMatrix(object, lightView, lightProjection))
                {
                    XMStoreFloat4x4(
                        &constants.directionalShadowViewProj[directionalShadowSlot],
                        XMMatrixTranspose(lightView * lightProjection));
                    gpuLight.shadowData = {
                        static_cast<float>(kShadowTypeDirectional),
                        static_cast<float>(directionalShadowSlot),
                        object.light->shadow.strength,
                        object.light->shadow.bias};
                    ++directionalShadowSlot;
                }
            }
            else if (object.light->type == LightType::Point && pointShadowSlot < kMaxLightCount)
            {
                gpuLight.shadowData = {
                    static_cast<float>(kShadowTypePoint),
                    static_cast<float>(pointShadowSlot),
                    object.light->shadow.strength,
                    object.light->shadow.bias};
                ++pointShadowSlot;
            }
        }
    }

    std::memcpy(mappedLightingConstants_, &constants, sizeof(constants));
}

void RendererDX12::UpdateShadowConstants(const Scene& scene, const XMMATRIX& lightView, const XMMATRIX& lightProjection, uint32_t passIndex) const
{
    const XMMATRIX view = XMMatrixTranspose(lightView);
    const XMMATRIX proj = XMMatrixTranspose(lightProjection);
    const std::vector<SceneObject>& objects = scene.Objects();
    const uint32_t objectCount = std::min<uint32_t>(static_cast<uint32_t>(objects.size()), kMaxObjectCount);

    for (uint32_t objectIndex = 0; objectIndex < objectCount; ++objectIndex)
    {
        const SceneObject& object = objects[objectIndex];
        ObjectConstants constants{};
        XMStoreFloat4x4(&constants.world, XMMatrixTranspose(object.transform.WorldMatrix()));
        XMStoreFloat4x4(&constants.view, view);
        XMStoreFloat4x4(&constants.proj, proj);

        std::memcpy(
            reinterpret_cast<std::byte*>(mappedShadowObjectConstants_) +
                static_cast<size_t>(passIndex) * kObjectConstantBufferSize +
                objectIndex * kPerObjectConstantBufferSize,
            &constants,
            sizeof(constants));
    }
}

bool RendererDX12::BuildDirectionalShadowMatrix(const SceneObject& lightObject, XMMATRIX& lightView, XMMATRIX& lightProjection) const
{
    if (!lightObject.light.has_value() || lightObject.light->type != LightType::Directional)
    {
        return false;
    }

    const XMMATRIX rotation = XMMatrixRotationRollPitchYaw(
        lightObject.transform.rotationEuler.x,
        lightObject.transform.rotationEuler.y,
        lightObject.transform.rotationEuler.z);
    XMVECTOR lightDirection = XMVector3Normalize(XMVector3TransformNormal(XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f), rotation));
    XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    if (std::abs(XMVectorGetX(XMVector3Dot(lightDirection, up))) > 0.92f)
    {
        up = XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f);
    }

    constexpr float sceneRadius = 18.0f;
    const XMVECTOR center = XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f);
    const XMVECTOR eye = center - lightDirection * sceneRadius;
    lightView = XMMatrixLookToLH(eye, lightDirection, up);
    lightProjection = XMMatrixOrthographicLH(sceneRadius * 2.0f, sceneRadius * 2.0f, 0.1f, sceneRadius * 3.0f);
    return true;
}

void RendererDX12::BuildPointShadowMatrices(
    const SceneObject& lightObject,
    std::array<XMMATRIX, kPointShadowFaceCount>& lightViews,
    XMMATRIX& lightProjection) const
{
    const XMVECTOR position = XMLoadFloat3(&lightObject.transform.position);
    const std::array<XMVECTOR, kPointShadowFaceCount> directions = {
        XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f),
        XMVectorSet(-1.0f, 0.0f, 0.0f, 0.0f),
        XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f),
        XMVectorSet(0.0f, -1.0f, 0.0f, 0.0f),
        XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f),
        XMVectorSet(0.0f, 0.0f, -1.0f, 0.0f)};
    const std::array<XMVECTOR, kPointShadowFaceCount> ups = {
        XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f),
        XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f),
        XMVectorSet(0.0f, 0.0f, -1.0f, 0.0f),
        XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f),
        XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f),
        XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f)};

    for (uint32_t face = 0; face < kPointShadowFaceCount; ++face)
    {
        lightViews[face] = XMMatrixLookToLH(position, directions[face], ups[face]);
    }

    const float range = lightObject.light.has_value() ? std::max(lightObject.light->range, kPointShadowNearPlane + 0.1f) : 1.0f;
    lightProjection = XMMatrixPerspectiveFovLH(XM_PIDIV2, 1.0f, kPointShadowNearPlane, range);
}

void RendererDX12::PopulateCommandList(const Scene& scene, ImDrawData* imguiDrawData)
{
    ThrowIfFailed(commandAllocators_[frameIndex_]->Reset(), "Failed to reset command allocator.");
    ThrowIfFailed(commandList_->Reset(commandAllocators_[frameIndex_].Get(), nullptr), "Failed to reset graphics command list.");

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

    RenderShadowMaps(scene);
    commandList_->RSSetViewports(1, &viewport_);
    commandList_->RSSetScissorRects(1, &scissorRect_);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    rtvHandle.ptr += static_cast<SIZE_T>(frameIndex_) * rtvDescriptorSize_;
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = dsvHeap_->GetCPUDescriptorHandleForHeapStart();

    if (renderPipelineMode_ == RenderPipelineMode::Deferred)
    {
        RenderDeferred(scene, rtvHandle, dsvHandle);
    }
    else
    {
        RenderForward(scene, rtvHandle, dsvHandle);
    }

    RenderImGui(imguiDrawData);

    D3D12_RESOURCE_BARRIER endBarrier = beginBarrier;
    endBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    endBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    commandList_->ResourceBarrier(1, &endBarrier);

    ThrowIfFailed(commandList_->Close(), "Failed to close command list.");
}

void RendererDX12::RenderShadowMaps(const Scene& scene)
{
    uint32_t directionalShadowSlot = 0;
    uint32_t pointShadowSlot = 0;
    uint32_t passIndex = 0;
    bool hasDirectionalShadows = false;
    bool hasPointShadows = false;

    for (const SceneObject& object : scene.Objects())
    {
        if (!object.light.has_value() || !object.light->enabled || !object.light->shadow.enabled)
        {
            continue;
        }

        if (object.light->type == LightType::Directional && directionalShadowSlot < kMaxLightCount)
        {
            hasDirectionalShadows = true;
            RenderDirectionalShadow(scene, directionalShadowSlot++, passIndex++);
        }
        else if (object.light->type == LightType::Point && pointShadowSlot < kMaxLightCount)
        {
            hasPointShadows = true;
            RenderPointShadow(scene, object, pointShadowSlot++, passIndex);
            passIndex += kPointShadowFaceCount;
        }
    }

    (void)hasDirectionalShadows;
    (void)hasPointShadows;
}

void RendererDX12::RenderDirectionalShadow(const Scene& scene, uint32_t shadowSlot, uint32_t passIndex)
{
    const SceneObject* lightObject = nullptr;
    uint32_t currentSlot = 0;
    for (const SceneObject& object : scene.Objects())
    {
        if (!object.light.has_value() || !object.light->enabled || !object.light->shadow.enabled || object.light->type != LightType::Directional)
        {
            continue;
        }

        if (currentSlot == shadowSlot)
        {
            lightObject = &object;
            break;
        }
        ++currentSlot;
    }

    if (lightObject == nullptr)
    {
        return;
    }

    XMMATRIX shadowView{};
    XMMATRIX shadowProjection{};
    if (!BuildDirectionalShadowMatrix(*lightObject, shadowView, shadowProjection))
    {
        return;
    }
    UpdateShadowConstants(scene, shadowView, shadowProjection, passIndex);

    D3D12_RESOURCE_BARRIER toDepthWrite{};
    toDepthWrite.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toDepthWrite.Transition.pResource = directionalShadowMap_.Get();
    toDepthWrite.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    toDepthWrite.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    toDepthWrite.Transition.Subresource = shadowSlot;
    commandList_->ResourceBarrier(1, &toDepthWrite);

    D3D12_CPU_DESCRIPTOR_HANDLE shadowDsv = dsvHeap_->GetCPUDescriptorHandleForHeapStart();
    shadowDsv.ptr += static_cast<SIZE_T>(1 + shadowSlot) * dsvDescriptorSize_;

    commandList_->SetPipelineState(shadowPipelineState_.Get());
    commandList_->RSSetViewports(1, &shadowViewport_);
    commandList_->RSSetScissorRects(1, &shadowScissorRect_);
    commandList_->OMSetRenderTargets(0, nullptr, FALSE, &shadowDsv);
    commandList_->ClearDepthStencilView(shadowDsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    DrawSceneMeshesForShadow(scene, passIndex);

    D3D12_RESOURCE_BARRIER toShaderResource = toDepthWrite;
    toShaderResource.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    toShaderResource.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    commandList_->ResourceBarrier(1, &toShaderResource);
}

void RendererDX12::RenderPointShadow(const Scene& scene, const SceneObject& lightObject, uint32_t shadowSlot, uint32_t firstPassIndex)
{
    std::array<XMMATRIX, kPointShadowFaceCount> shadowViews{};
    XMMATRIX shadowProjection{};
    BuildPointShadowMatrices(lightObject, shadowViews, shadowProjection);

    for (uint32_t face = 0; face < kPointShadowFaceCount; ++face)
    {
        const uint32_t passIndex = firstPassIndex + face;
        const uint32_t faceSlice = shadowSlot * kPointShadowFaceCount + face;
        UpdateShadowConstants(scene, shadowViews[face], shadowProjection, passIndex);

        D3D12_RESOURCE_BARRIER toDepthWrite{};
        toDepthWrite.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toDepthWrite.Transition.pResource = pointShadowMap_.Get();
        toDepthWrite.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        toDepthWrite.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        toDepthWrite.Transition.Subresource = faceSlice;
        commandList_->ResourceBarrier(1, &toDepthWrite);

        D3D12_CPU_DESCRIPTOR_HANDLE shadowDsv = dsvHeap_->GetCPUDescriptorHandleForHeapStart();
        shadowDsv.ptr += static_cast<SIZE_T>(1 + kMaxLightCount + faceSlice) * dsvDescriptorSize_;

        commandList_->SetPipelineState(pointShadowPipelineState_.Get());
        commandList_->RSSetViewports(1, &pointShadowViewport_);
        commandList_->RSSetScissorRects(1, &pointShadowScissorRect_);
        commandList_->OMSetRenderTargets(0, nullptr, FALSE, &shadowDsv);
        commandList_->ClearDepthStencilView(shadowDsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        DrawSceneMeshesForShadow(scene, passIndex);

        D3D12_RESOURCE_BARRIER toShaderResource = toDepthWrite;
        toShaderResource.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        toShaderResource.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        commandList_->ResourceBarrier(1, &toShaderResource);
    }
}

void RendererDX12::DrawSceneMeshesForShadow(const Scene& scene, uint32_t passIndex)
{
    commandList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList_->IASetVertexBuffers(0, 1, &vertexBufferView_);
    commandList_->IASetIndexBuffer(&indexBufferView_);

    const std::vector<SceneObject>& objects = scene.Objects();
    const uint32_t objectCount = std::min<uint32_t>(static_cast<uint32_t>(objects.size()), kMaxObjectCount);
    for (uint32_t objectIndex = 0; objectIndex < objectCount; ++objectIndex)
    {
        const SceneObject& object = objects[objectIndex];
        if (!object.mesh.has_value())
        {
            continue;
        }

        const MeshRange* mesh = FindMeshRange(object.mesh->meshAssetId);
        if (mesh == nullptr || mesh->indexCount == 0)
        {
            continue;
        }

        commandList_->SetGraphicsRootConstantBufferView(
            0,
            shadowObjectConstantBuffer_->GetGPUVirtualAddress() +
                static_cast<UINT64>(passIndex) * kObjectConstantBufferSize +
                static_cast<UINT64>(objectIndex) * kPerObjectConstantBufferSize);
        commandList_->DrawIndexedInstanced(mesh->indexCount, 1, mesh->startIndex, mesh->baseVertex, 0);
    }
}

void RendererDX12::RenderForward(const Scene& scene, D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle, D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle)
{
    commandList_->SetPipelineState(forwardPipelineState_.Get());
    commandList_->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

    const float clearColor[] = {0.09f, 0.11f, 0.16f, 1.0f};
    commandList_->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
    commandList_->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    D3D12_GPU_DESCRIPTOR_HANDLE textureHandle = cbvSrvHeap_->GetGPUDescriptorHandleForHeapStart();
    textureHandle.ptr += static_cast<SIZE_T>(kDiffuseSrvIndex) * cbvSrvDescriptorSize_;
    D3D12_GPU_DESCRIPTOR_HANDLE shadowHandle = cbvSrvHeap_->GetGPUDescriptorHandleForHeapStart();
    shadowHandle.ptr += static_cast<SIZE_T>(kDirectionalShadowSrvIndex) * cbvSrvDescriptorSize_;
    commandList_->SetGraphicsRootConstantBufferView(1, lightingConstantBuffer_->GetGPUVirtualAddress());
    commandList_->SetGraphicsRootDescriptorTable(2, textureHandle);
    commandList_->SetGraphicsRootDescriptorTable(3, shadowHandle);
    DrawSceneMeshes(scene);
}

void RendererDX12::RenderDeferred(const Scene& scene, D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle, D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle)
{
    std::array<D3D12_RESOURCE_BARRIER, kGBufferTargetCount> toRenderTargetBarriers{};
    for (uint32_t target = 0; target < kGBufferTargetCount; ++target)
    {
        toRenderTargetBarriers[target].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toRenderTargetBarriers[target].Transition.pResource = gbufferTargets_[target].Get();
        toRenderTargetBarriers[target].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        toRenderTargetBarriers[target].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        toRenderTargetBarriers[target].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    commandList_->ResourceBarrier(static_cast<UINT>(toRenderTargetBarriers.size()), toRenderTargetBarriers.data());

    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, kGBufferTargetCount> gbufferRtvs{};
    D3D12_CPU_DESCRIPTOR_HANDLE gbufferRtvHandle = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    gbufferRtvHandle.ptr += static_cast<SIZE_T>(kFrameCount) * rtvDescriptorSize_;
    for (uint32_t target = 0; target < kGBufferTargetCount; ++target)
    {
        gbufferRtvs[target] = gbufferRtvHandle;
        gbufferRtvHandle.ptr += rtvDescriptorSize_;
    }

    commandList_->SetPipelineState(gbufferPipelineState_.Get());
    commandList_->OMSetRenderTargets(static_cast<UINT>(gbufferRtvs.size()), gbufferRtvs.data(), FALSE, &dsvHandle);
    const float clearGBuffer[] = {0.0f, 0.0f, 0.0f, 1.0f};
    for (const D3D12_CPU_DESCRIPTOR_HANDLE& targetRtv : gbufferRtvs)
    {
        commandList_->ClearRenderTargetView(targetRtv, clearGBuffer, 0, nullptr);
    }
    commandList_->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    D3D12_GPU_DESCRIPTOR_HANDLE textureHandle = cbvSrvHeap_->GetGPUDescriptorHandleForHeapStart();
    textureHandle.ptr += static_cast<SIZE_T>(kDiffuseSrvIndex) * cbvSrvDescriptorSize_;
    D3D12_GPU_DESCRIPTOR_HANDLE shadowHandle = cbvSrvHeap_->GetGPUDescriptorHandleForHeapStart();
    shadowHandle.ptr += static_cast<SIZE_T>(kDirectionalShadowSrvIndex) * cbvSrvDescriptorSize_;
    commandList_->SetGraphicsRootConstantBufferView(1, lightingConstantBuffer_->GetGPUVirtualAddress());
    commandList_->SetGraphicsRootDescriptorTable(2, textureHandle);
    commandList_->SetGraphicsRootDescriptorTable(3, shadowHandle);
    DrawSceneMeshes(scene);

    std::array<D3D12_RESOURCE_BARRIER, kGBufferTargetCount> toShaderResourceBarriers{};
    for (uint32_t target = 0; target < kGBufferTargetCount; ++target)
    {
        toShaderResourceBarriers[target] = toRenderTargetBarriers[target];
        toShaderResourceBarriers[target].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        toShaderResourceBarriers[target].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }
    commandList_->ResourceBarrier(static_cast<UINT>(toShaderResourceBarriers.size()), toShaderResourceBarriers.data());

    commandList_->SetPipelineState(deferredLightingPipelineState_.Get());
    commandList_->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
    const float clearColor[] = {0.09f, 0.11f, 0.16f, 1.0f};
    commandList_->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

    D3D12_GPU_DESCRIPTOR_HANDLE gbufferSrvHandle = cbvSrvHeap_->GetGPUDescriptorHandleForHeapStart();
    gbufferSrvHandle.ptr += static_cast<SIZE_T>(kGBufferSrvStartIndex) * cbvSrvDescriptorSize_;
    commandList_->SetGraphicsRootConstantBufferView(1, lightingConstantBuffer_->GetGPUVirtualAddress());
    commandList_->SetGraphicsRootDescriptorTable(2, gbufferSrvHandle);
    commandList_->SetGraphicsRootDescriptorTable(3, shadowHandle);
    commandList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList_->IASetVertexBuffers(0, 0, nullptr);
    commandList_->IASetIndexBuffer(nullptr);
    commandList_->DrawInstanced(3, 1, 0, 0);
}

void RendererDX12::DrawSceneMeshes(const Scene& scene)
{
    commandList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList_->IASetVertexBuffers(0, 1, &vertexBufferView_);
    commandList_->IASetIndexBuffer(&indexBufferView_);

    const std::vector<SceneObject>& objects = scene.Objects();
    const uint32_t objectCount = std::min<uint32_t>(static_cast<uint32_t>(objects.size()), kMaxObjectCount);
    for (uint32_t objectIndex = 0; objectIndex < objectCount; ++objectIndex)
    {
        const SceneObject& object = objects[objectIndex];
        if (!object.mesh.has_value())
        {
            continue;
        }

        const MeshRange* mesh = FindMeshRange(object.mesh->meshAssetId);
        if (mesh == nullptr || mesh->indexCount == 0)
        {
            continue;
        }

        commandList_->SetGraphicsRootConstantBufferView(
            0,
            objectConstantBuffer_->GetGPUVirtualAddress() + static_cast<UINT64>(objectIndex) * kPerObjectConstantBufferSize);
        commandList_->DrawIndexedInstanced(mesh->indexCount, 1, mesh->startIndex, mesh->baseVertex, 0);
    }
}

void RendererDX12::RenderImGui(ImDrawData* imguiDrawData)
{
    if (imguiDrawData == nullptr)
    {
        return;
    }

    ID3D12DescriptorHeap* imguiDescriptorHeaps[] = {imguiSrvHeap_.Get()};
    commandList_->SetDescriptorHeaps(1, imguiDescriptorHeaps);
    ImGui_ImplDX12_RenderDrawData(imguiDrawData, commandList_.Get());
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
