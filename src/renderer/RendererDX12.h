#pragma once

#include "engine/Camera.h"
#include "engine/Scene.h"

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct ImDrawData;

namespace engine
{
class Window;

enum class RenderPipelineMode
{
    Forward,
    Deferred
};

struct MeshAssetInfo
{
    uint32_t id = kInvalidMeshAssetId;
    std::wstring sourcePath{};
    std::string name{};
    bool missing = false;
};

class RendererDX12
{
public:
    RendererDX12() = default;
    ~RendererDX12();

    bool Initialize(Window& window);
    bool InitializeImGui();
    bool LoadModelFromFile(const std::wstring& path);
    uint32_t LoadMeshAssetFromFile(const std::wstring& path);
    const MeshAssetInfo* FindMeshAsset(uint32_t id) const;
    bool LoadTextureFromFile(const std::wstring& path);
    void BeginImGuiFrame();
    void Resize(uint32_t width, uint32_t height);
    void Render(const Scene& scene, const Camera& camera, ImDrawData* imguiDrawData = nullptr);
    void SetRenderPipelineMode(RenderPipelineMode mode) { renderPipelineMode_ = mode; }
    RenderPipelineMode Mode() const { return renderPipelineMode_; }
    void Shutdown();

private:
    enum class GBufferTarget : uint32_t
    {
        Albedo,
        Normal,
        WorldPosition,
        Count
    };

    struct GpuLight
    {
        DirectX::XMFLOAT4 positionRange{0.0f, 0.0f, 0.0f, 0.0f};
        DirectX::XMFLOAT4 directionType{0.45f, -1.0f, 0.35f, 0.0f};
        DirectX::XMFLOAT4 colorIntensity{1.0f, 0.96f, 0.88f, 1.0f};
        DirectX::XMFLOAT4 shadowData{0.0f, 0.0f, 0.0f, 0.0035f};
    };

    struct MeshRange
    {
        uint32_t indexCount = 0;
        uint32_t startIndex = 0;
        int32_t baseVertex = 0;
    };

    struct Vertex
    {
        float position[3];
        float normal[3];
        float uv[2];
    };

    struct MeshAsset
    {
        MeshAssetInfo info{};
        MeshRange range{};
    };

    struct alignas(256) ObjectConstants
    {
        DirectX::XMFLOAT4X4 world{};
        DirectX::XMFLOAT4X4 view{};
        DirectX::XMFLOAT4X4 proj{};
        DirectX::XMFLOAT3 cameraPos{};
        float padding0 = 0.0f;
        DirectX::XMFLOAT4 tint{1.0f, 1.0f, 1.0f, 0.0f};
    };

    struct alignas(256) LightingConstants
    {
        DirectX::XMFLOAT4 ambientColor{0.0f, 0.0f, 0.0f, 1.0f};
        uint32_t lightCount = 0;
        DirectX::XMFLOAT3 padding0{};
        std::array<GpuLight, 16> lights{};
        std::array<DirectX::XMFLOAT4X4, 16> directionalShadowViewProj{};
    };

    static_assert(sizeof(GpuLight) == 64, "GpuLight must match the HLSL layout.");
    static_assert(offsetof(LightingConstants, lights) == 32, "LightingConstants must match the HLSL layout.");

    static constexpr uint32_t kFrameCount = 2;
    static constexpr uint32_t kMaxObjectCount = 128;
    static constexpr uint32_t kMaxLightCount = 16;
    static constexpr uint32_t kGBufferTargetCount = static_cast<uint32_t>(GBufferTarget::Count);
    static constexpr uint32_t kPointShadowFaceCount = 6;
    static constexpr uint32_t kMaxShadowPassCount = kMaxLightCount + kMaxLightCount * kPointShadowFaceCount;
    static constexpr uint32_t kPerObjectConstantBufferSize = (sizeof(ObjectConstants) + 255u) & ~255u;
    static constexpr uint32_t kObjectConstantBufferSize = kPerObjectConstantBufferSize * kMaxObjectCount;
    static constexpr uint32_t kShadowObjectConstantBufferSize = kObjectConstantBufferSize * kMaxShadowPassCount;
    static constexpr uint32_t kLightingConstantBufferSize = (sizeof(LightingConstants) + 255u) & ~255u;
    static constexpr uint32_t kShadowMapSize = 2048;
    static constexpr uint32_t kPointShadowMapSize = 1024;

    bool InitializeDeviceResources();
    bool CreateSwapChainResources();
    bool CreateDepthStencil(uint32_t width, uint32_t height);
    bool CreateRootSignatureAndPipelines();
    bool CreateForwardPipeline();
    bool CreateDeferredPipelines();
    bool CreateShadowPipeline();
    bool CreateGBufferResources(uint32_t width, uint32_t height);
    bool CreateShadowResources();
    bool CreateGeometry();
    bool CreateConstantBuffers();
    bool CreateCheckerboardTexture();
    bool CreateTextureFromRGBA(
        uint32_t width,
        uint32_t height,
        const std::vector<uint8_t>& rgbaPixels,
        const char* debugName);
    bool LoadObjMesh(
        const std::wstring& path,
        std::vector<Vertex>& outVertices,
        std::vector<uint16_t>& outIndices);
    bool LoadImportedMesh(
        const std::wstring& path,
        std::vector<Vertex>& outVertices,
        std::vector<uint16_t>& outIndices);
    bool CreateGeometryBuffers(const char* debugName);
    const MeshRange* FindMeshRange(uint32_t meshAssetId) const;
    const MeshAsset* FindMeshAssetInternal(uint32_t id) const;
    bool AllocateImGuiDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE* cpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE* gpuHandle);
    void FreeImGuiDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle);
    void UpdateConstants(const Scene& scene, const Camera& camera) const;
    void UpdateLightingConstants(const Scene& scene) const;
    void UpdateShadowConstants(const Scene& scene, const DirectX::XMMATRIX& lightView, const DirectX::XMMATRIX& lightProjection, uint32_t passIndex) const;
    bool BuildDirectionalShadowMatrix(const SceneObject& lightObject, DirectX::XMMATRIX& lightView, DirectX::XMMATRIX& lightProjection) const;
    void BuildPointShadowMatrices(const SceneObject& lightObject, std::array<DirectX::XMMATRIX, kPointShadowFaceCount>& lightViews, DirectX::XMMATRIX& lightProjection) const;
    void PopulateCommandList(const Scene& scene, ImDrawData* imguiDrawData);
    void RenderShadowMaps(const Scene& scene);
    void RenderDirectionalShadow(const Scene& scene, uint32_t shadowSlot, uint32_t passIndex);
    void RenderPointShadow(const Scene& scene, const SceneObject& lightObject, uint32_t shadowSlot, uint32_t firstPassIndex);
    void DrawSceneMeshesForShadow(const Scene& scene, uint32_t passIndex);
    void RenderForward(const Scene& scene, D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle, D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle);
    void RenderDeferred(const Scene& scene, D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle, D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle);
    void DrawSceneMeshes(const Scene& scene);
    void RenderImGui(ImDrawData* imguiDrawData);
    void MoveToNextFrame();
    void WaitForGpu();
    void RecreateRenderTargets();
    std::vector<std::byte> LoadFileBytes(const wchar_t* path) const;

    Window* window_ = nullptr;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    bool initialized_ = false;

    Microsoft::WRL::ComPtr<IDXGIFactory6> factory_;
    Microsoft::WRL::ComPtr<ID3D12Device2> device_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> commandQueue_;
    Microsoft::WRL::ComPtr<IDXGISwapChain4> swapChain_;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList_;
    std::array<Microsoft::WRL::ComPtr<ID3D12CommandAllocator>, kFrameCount> commandAllocators_{};
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, kFrameCount> renderTargets_{};
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtvHeap_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> dsvHeap_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> cbvSrvHeap_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> imguiSrvHeap_;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    HANDLE fenceEvent_ = nullptr;
    std::array<uint64_t, kFrameCount> fenceValues_{};
    uint32_t frameIndex_ = 0;
    uint64_t nextFenceValue_ = 1;
    UINT rtvDescriptorSize_ = 0;
    UINT cbvSrvDescriptorSize_ = 0;
    UINT dsvDescriptorSize_ = 0;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> forwardPipelineState_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> gbufferPipelineState_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> deferredLightingPipelineState_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> shadowPipelineState_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pointShadowPipelineState_;
    Microsoft::WRL::ComPtr<ID3D12Resource> vertexBuffer_;
    Microsoft::WRL::ComPtr<ID3D12Resource> indexBuffer_;
    Microsoft::WRL::ComPtr<ID3D12Resource> objectConstantBuffer_;
    Microsoft::WRL::ComPtr<ID3D12Resource> shadowObjectConstantBuffer_;
    Microsoft::WRL::ComPtr<ID3D12Resource> lightingConstantBuffer_;
    Microsoft::WRL::ComPtr<ID3D12Resource> depthStencil_;
    Microsoft::WRL::ComPtr<ID3D12Resource> directionalShadowMap_;
    Microsoft::WRL::ComPtr<ID3D12Resource> pointShadowMap_;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, kGBufferTargetCount> gbufferTargets_{};
    Microsoft::WRL::ComPtr<ID3D12Resource> texture_;
    Microsoft::WRL::ComPtr<ID3D12Resource> textureUpload_;
    D3D12_VERTEX_BUFFER_VIEW vertexBufferView_{};
    D3D12_INDEX_BUFFER_VIEW indexBufferView_{};
    ObjectConstants* mappedObjectConstants_ = nullptr;
    ObjectConstants* mappedShadowObjectConstants_ = nullptr;
    LightingConstants* mappedLightingConstants_ = nullptr;
    D3D12_VIEWPORT viewport_{};
    D3D12_RECT scissorRect_{};
    D3D12_VIEWPORT shadowViewport_{};
    D3D12_RECT shadowScissorRect_{};
    D3D12_VIEWPORT pointShadowViewport_{};
    D3D12_RECT pointShadowScissorRect_{};
    MeshRange cubeMesh_{};
    MeshRange sphereMesh_{};
    std::vector<Vertex> meshVertices_{};
    std::vector<uint16_t> meshIndices_{};
    std::vector<MeshAsset> meshAssets_{};
    uint32_t nextMeshAssetId_ = kSphereMeshAssetId + 1;
    std::wstring loadedModelPath_{};
    std::wstring loadedTexturePath_{};
    RenderPipelineMode renderPipelineMode_ = RenderPipelineMode::Forward;
    bool imguiInitialized_ = false;
    bool imguiDescriptorAllocated_ = false;
};
} // namespace engine
