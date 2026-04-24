#pragma once

#include "engine/Camera.h"
#include "engine/Scene.h"

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

struct ImDrawData;

namespace engine
{
class Window;

class RendererDX12
{
public:
    RendererDX12() = default;
    ~RendererDX12();

    bool Initialize(Window& window);
    bool InitializeImGui();
    bool LoadModelFromFile(const std::wstring& path);
    bool LoadTextureFromFile(const std::wstring& path);
    void BeginImGuiFrame();
    void Resize(uint32_t width, uint32_t height);
    void Render(const Scene& scene, const Camera& camera, ImDrawData* imguiDrawData = nullptr);
    void Shutdown();

private:
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

    struct alignas(256) SceneConstants
    {
        DirectX::XMFLOAT4X4 world{};
        DirectX::XMFLOAT4X4 view{};
        DirectX::XMFLOAT4X4 proj{};
        DirectX::XMFLOAT3 cameraPos{};
        float padding0 = 0.0f;
        DirectX::XMFLOAT3 lightDir{0.45f, -1.0f, 0.35f};
        float padding1 = 0.0f;
    };

    static constexpr uint32_t kFrameCount = 2;
    static constexpr uint32_t kObjectCount = 2;
    static constexpr uint32_t kPerObjectConstantBufferSize = (sizeof(SceneConstants) + 255u) & ~255u;
    static constexpr uint32_t kConstantBufferSize = kPerObjectConstantBufferSize * kObjectCount;

    bool InitializeDeviceResources();
    bool CreateSwapChainResources();
    bool CreateDepthStencil(uint32_t width, uint32_t height);
    bool CreateRootSignatureAndPipeline();
    bool CreateGeometry();
    bool CreateConstantBuffer();
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
    bool CreateGeometryBuffers(
        const std::vector<Vertex>& primaryVertices,
        const std::vector<uint16_t>& primaryIndices,
        const char* debugName);
    bool AllocateImGuiDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE* cpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE* gpuHandle);
    void FreeImGuiDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle);
    void UpdateConstants(const Scene& scene, const Camera& camera) const;
    void PopulateCommandList(ImDrawData* imguiDrawData);
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

    Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipelineState_;
    Microsoft::WRL::ComPtr<ID3D12Resource> vertexBuffer_;
    Microsoft::WRL::ComPtr<ID3D12Resource> indexBuffer_;
    Microsoft::WRL::ComPtr<ID3D12Resource> constantBuffer_;
    Microsoft::WRL::ComPtr<ID3D12Resource> depthStencil_;
    Microsoft::WRL::ComPtr<ID3D12Resource> texture_;
    Microsoft::WRL::ComPtr<ID3D12Resource> textureUpload_;
    D3D12_VERTEX_BUFFER_VIEW vertexBufferView_{};
    D3D12_INDEX_BUFFER_VIEW indexBufferView_{};
    SceneConstants* mappedConstants_ = nullptr;
    D3D12_VIEWPORT viewport_{};
    D3D12_RECT scissorRect_{};
    MeshRange cubeMesh_{};
    MeshRange sphereMesh_{};
    std::wstring loadedModelPath_{};
    std::wstring loadedTexturePath_{};
    bool imguiInitialized_ = false;
    bool imguiDescriptorAllocated_ = false;
};
} // namespace engine
