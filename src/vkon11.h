// VKOn11 - a Vulkan 1.0 installable client driver implemented on top of Direct3D 11.
// Written for feature-level 11_0 hardware that never received a native Vulkan
// driver (AMD Radeon HD 5000/6000 "Evergreen"/"Northern Islands", e.g. HD 6570).
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi.h>
// windows.h turns these into ...A/...W macros, which would rename our entry points.
#undef CreateSemaphore
#undef CreateEvent

#define VK_USE_PLATFORM_WIN32_KHR
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <vulkan/vk_icd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace vkon11 {

void logf(const char* fmt, ...);
#define VKON_LOG(...) ::vkon11::logf(__VA_ARGS__)

// ---------------------------------------------------------------------------
// Minimal COM smart pointer (mingw has no usable wrl/client.h).
template <class T> class ComPtr {
    T* p_ = nullptr;
public:
    ComPtr() = default;
    ComPtr(T* p) : p_(p) { if (p_) p_->AddRef(); }
    ComPtr(const ComPtr& o) : p_(o.p_) { if (p_) p_->AddRef(); }
    ComPtr(ComPtr&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
    ~ComPtr() { reset(); }
    ComPtr& operator=(const ComPtr& o) { if (this != &o) { reset(); p_ = o.p_; if (p_) p_->AddRef(); } return *this; }
    ComPtr& operator=(ComPtr&& o) noexcept { if (this != &o) { reset(); p_ = o.p_; o.p_ = nullptr; } return *this; }
    void reset() { if (p_) { p_->Release(); p_ = nullptr; } }
    T* get() const { return p_; }
    T* operator->() const { return p_; }
    T** put() { reset(); return &p_; }
    explicit operator bool() const { return p_ != nullptr; }
};

template <class T, class H> inline T* from(H h) { return reinterpret_cast<T*>((uintptr_t)h); }
template <class H, class T> inline H to(T* p) { return (H)(uintptr_t)p; }

// ---------------------------------------------------------------------------
// Formats
enum FormatFlags : uint32_t {
    FMT_DEPTH = 1, FMT_STENCIL = 2, FMT_COMPRESSED = 4, FMT_INT = 8, FMT_VERTEX_ONLY = 16,
};
struct FormatInfo {
    VkFormat vk;
    DXGI_FORMAT dxgi;      // format used for render target / depth-stencil / vertex input views
    DXGI_FORMAT typeless;  // resource format (differs from dxgi for depth formats)
    DXGI_FORMAT srv;       // format used when sampled
    uint8_t bytes;         // bytes per texel, or per block for compressed formats
    uint8_t block;         // block dimension (1 or 4)
    uint32_t flags;
};
const FormatInfo* findFormat(VkFormat f);
const FormatInfo* formatTable(size_t* count);

// ---------------------------------------------------------------------------
struct Instance;
struct Device;
struct Pipeline;

struct PhysicalDevice {
    VK_LOADER_DATA loader;
    Instance* instance = nullptr;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC desc{};
    ComPtr<ID3D11Device> d3d;
    ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
    std::mutex ctxMutex; // the immediate context is shared by every VkDevice on this adapter
};

struct Instance {
    VK_LOADER_DATA loader;
    std::vector<std::unique_ptr<PhysicalDevice>> physicalDevices;
    bool enumerated = false;
};

struct Queue {
    VK_LOADER_DATA loader;
    Device* device = nullptr;
};

// Every VkDeviceMemory is backed by CPU pages ("shadow"). Buffers treat the
// shadow as the source of truth and lazily mirror dirty pages into D3D11
// buffers; images own their D3D11 texture and ignore the bound memory.
struct Memory {
    uint8_t* base = nullptr;
    VkDeviceSize size = 0;
    bool hostVisible = false;
    bool watch = false;     // allocated with MEM_WRITE_WATCH
    bool committed = false; // whole range committed
    std::vector<uint32_t> pageEpoch;
    uint32_t epoch = 1;
    void commit(VkDeviceSize off, VkDeviceSize len);
    void scan(VkDeviceSize off, VkDeviceSize len);      // pull OS write-watch info into pageEpoch
    void markDirty(VkDeviceSize off, VkDeviceSize len); // driver wrote to the shadow itself
};

struct Buffer {
    VkBufferCreateInfo ci{};
    Memory* mem = nullptr;
    VkDeviceSize memOffset = 0;
    ComPtr<ID3D11Buffer> d3d;    // vertex/index (and raw SRV/UAV) mirror
    ComPtr<ID3D11UnorderedAccessView> uav;
    ComPtr<ID3D11ShaderResourceView> srv;
    uint32_t syncedEpoch = 0;
    uint64_t lastSyncSubmit = 0;
    uint32_t version = 0;         // bumped whenever the driver writes the shadow
    bool gpuWritten = false;      // d3d buffer has data newer than the shadow
    uint8_t* data() const { return mem ? mem->base + memOffset : nullptr; }
};

struct Image {
    VkImageCreateInfo ci{};
    const FormatInfo* fmt = nullptr;
    ComPtr<ID3D11Resource> tex;
    UINT sub(uint32_t mip, uint32_t layer) const { return mip + layer * ci.mipLevels; }
};

struct ImageView {
    Image* image = nullptr;
    VkImageViewCreateInfo ci{};
    const FormatInfo* fmt = nullptr;
    uint32_t levelCount = 1, layerCount = 1;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11UnorderedAccessView> uav;
};

struct BufferView {
    Buffer* buffer = nullptr;
    VkBufferViewCreateInfo ci{};
};

struct Sampler {
    ComPtr<ID3D11SamplerState> d3d;
};

struct ShaderModule {
    std::vector<uint32_t> code;
};

struct SetBinding {
    uint32_t binding = 0;
    VkDescriptorType type = VK_DESCRIPTOR_TYPE_SAMPLER;
    uint32_t count = 0;
    VkShaderStageFlags stages = 0;
    uint32_t offset = 0;    // index of first descriptor inside the set
    uint32_t dynIndex = 0;  // index of first dynamic offset (dynamic buffers only)
    std::vector<Sampler*> immutableSamplers;
};
struct SetLayoutData {
    std::vector<SetBinding> bindings;
    uint32_t descriptorCount = 0;
    uint32_t dynamicCount = 0;
};
struct DescriptorSetLayout {
    std::shared_ptr<SetLayoutData> data;
};

struct Descriptor {
    Buffer* buffer = nullptr;
    VkDeviceSize offset = 0, range = 0;
    ImageView* view = nullptr;
    Sampler* sampler = nullptr;
    BufferView* bufferView = nullptr;
};
struct DescriptorPool;
struct DescriptorSet {
    std::shared_ptr<SetLayoutData> layout;
    std::vector<Descriptor> desc;
    DescriptorPool* pool = nullptr;
};
struct DescriptorPool {
    std::vector<DescriptorSet*> sets;
};

// D3D11 shader stages we emit code for.
enum Stage { ST_VS = 0, ST_PS = 1, ST_CS = 2, ST_COUNT = 3 };
static const UINT SLOT_PUSH_CONSTANTS = 13; // cbuffer register reserved for push constants
static const UINT SLOT_VERTEX_INFO = 12;    // cbuffer register reserved for base vertex/instance
static const UINT MAX_USER_CBV = 12;
static const UINT MAX_SRV = 64;
static const UINT MAX_SAMPLER = 16;
static const UINT MAX_UAV = 8;

struct SlotMap {
    uint32_t set = 0;
    uint32_t bindingIndex = 0; // index into SetLayoutData::bindings
    UINT cbv = 0, srv = 0, sampler = 0, uav = 0;
};
struct PipelineLayoutData {
    std::vector<std::shared_ptr<SetLayoutData>> sets;
    std::vector<SlotMap> slots[ST_COUNT];
    UINT cbvCount[ST_COUNT]{}, srvCount[ST_COUNT]{}, samplerCount[ST_COUNT]{}, uavCount[ST_COUNT]{};
};
struct PipelineLayout {
    std::shared_ptr<PipelineLayoutData> data;
};

struct SubpassData {
    std::vector<VkAttachmentReference> color, resolve, input;
    VkAttachmentReference depth{VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_UNDEFINED};
};
struct RenderPass {
    std::vector<VkAttachmentDescription> attachments;
    std::vector<SubpassData> subpasses;
};
struct Framebuffer {
    std::vector<ImageView*> views;
    uint32_t width = 0, height = 0, layers = 1;
};

struct Pipeline {
    bool compute = false;
    std::shared_ptr<PipelineLayoutData> layout;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11ComputeShader> cs;
    ComPtr<ID3D11InputLayout> inputLayout;
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    ComPtr<ID3D11BlendState> blend;
    float blendConstants[4]{};
    UINT sampleMask = 0xffffffff;
    D3D11_RASTERIZER_DESC rsDesc{};
    D3D11_DEPTH_STENCIL_DESC dsDesc{};
    UINT stencilRef = 0;
    ComPtr<ID3D11DepthStencilState> dsState; // cached for the non-dynamic case
    ComPtr<ID3D11RasterizerState> rsState;
    UINT strides[32]{};
    uint32_t vbMask = 0;        // vertex bindings referenced by the pipeline
    uint32_t dynamicMask = 0;   // bit per VkDynamicState (0..8)
    std::vector<VkViewport> viewports;
    std::vector<VkRect2D> scissors;
    bool usesVertexInfo = false;
    bool rasterDiscard = false;
};

struct CommandPool {
    std::vector<struct CommandBuffer*> buffers;
};
typedef std::function<void(Device&)> Cmd;
struct CommandBuffer {
    VK_LOADER_DATA loader;
    Device* device = nullptr;
    CommandPool* pool = nullptr;
    std::vector<Cmd> cmds;
};

struct Fence { bool signaled = false; };
struct Semaphore { int dummy = 0; };
struct Event { bool set = false; };
struct QueryPool {
    VkQueryType type = VK_QUERY_TYPE_OCCLUSION;
    std::vector<ComPtr<ID3D11Query>> queries;
    std::vector<uint8_t> active;
};

struct Swapchain {
    HWND hwnd = nullptr;
    ComPtr<IDXGISwapChain> dxgi;
    std::vector<Image*> images;
    VkExtent2D extent{};
    UINT syncInterval = 1;
    uint32_t next = 0;
};

// Replay-time state (what the Vulkan command stream has bound so far).
struct ExecState {
    Pipeline* pipeline[2]{};                 // [0]=graphics, [1]=compute
    DescriptorSet* sets[2][8]{};
    std::vector<uint32_t> dynOffsets[2][8];
    Buffer* vb[32]{};
    VkDeviceSize vbOffset[32]{};
    Buffer* ib = nullptr;
    VkDeviceSize ibOffset = 0;
    VkIndexType ibType = VK_INDEX_TYPE_UINT16;
    std::vector<VkViewport> viewports;
    std::vector<VkRect2D> scissors;
    float blendConstants[4]{};
    uint32_t stencilRef = 0, stencilCompareMask = 0xff, stencilWriteMask = 0xff;
    float depthBias[3]{};
    uint8_t push[256]{};
    RenderPass* rp = nullptr;
    Framebuffer* fb = nullptr;
    uint32_t subpass = 0;
    VkRect2D area{};
    std::vector<VkClearValue> clears;
    std::vector<uint8_t> cleared;
    bool dirtyPipeline = true, dirtyDynamic = true, dirtyVB = true, dirtyIB = true, dirtyDesc[2] = {true, true},
         dirtyPush = true;
    int lastBaseVertex = -1, lastBaseInstance = -1;
    float yflip = -1.0f;    // -1: flip clip-space Y (normal Vulkan viewport), +1: negative-height viewport
    float lastYflip = 0.0f; // value currently stored in the vertex info cbuffer (0 = unknown)
    void invalidate() {
        dirtyPipeline = dirtyDynamic = dirtyVB = dirtyIB = dirtyDesc[0] = dirtyDesc[1] = dirtyPush = true;
        lastBaseVertex = lastBaseInstance = -1;
        lastYflip = 0.0f;
    }
};

struct CbKey {
    Buffer* b; VkDeviceSize off, range; uint32_t version;
    bool operator<(const CbKey& o) const {
        if (b != o.b) return b < o.b;
        if (off != o.off) return off < o.off;
        if (range != o.range) return range < o.range;
        return version < o.version;
    }
};

struct Device {
    VK_LOADER_DATA loader;
    PhysicalDevice* phys = nullptr;
    ID3D11Device* d3d = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    ComPtr<ID3D11DeviceContext1> ctx1;
    Queue queue;
    VkPhysicalDeviceFeatures enabled{};
    uint64_t submitId = 1;
    ExecState st;

    // constant buffer pool: bucket (log2 size) -> free buffers
    std::vector<ComPtr<ID3D11Buffer>> cbFree[17];
    std::vector<std::pair<int, ComPtr<ID3D11Buffer>>> cbUsed;
    std::map<CbKey, ID3D11Buffer*> cbCache;
    ComPtr<ID3D11Buffer> pushCB, vertexInfoCB, blitCB;
    std::vector<Buffer*> gpuWritten;        // storage buffers written by the GPU during this submit
    std::vector<Buffer*> pendingUavBuffers; // storage buffers bound for the next dispatch
    std::vector<ComPtr<ID3D11UnorderedAccessView>> tempUavs;

    // internal blit pipeline
    ComPtr<ID3D11VertexShader> blitVS;
    ComPtr<ID3D11PixelShader> blitPS;
    ComPtr<ID3D11SamplerState> blitLinear, blitPoint;
    ComPtr<ID3D11RasterizerState> blitRS;
    ComPtr<ID3D11DepthStencilState> blitDS;
    ComPtr<ID3D11BlendState> blitBlend;
    // internal "clear a rectangle" pipeline (D3D11 can only clear whole views)
    ComPtr<ID3D11VertexShader> clearVS;
    ComPtr<ID3D11PixelShader> clearPS[3]; // float, sint, uint outputs
    ComPtr<ID3D11RasterizerState> clearRS;
    ComPtr<ID3D11Buffer> clearCB;

    ID3D11Buffer* uniformCB(Buffer* b, VkDeviceSize off, VkDeviceSize range);
    void endSubmit();
};

// ---------------------------------------------------------------------------
// Helpers implemented across translation units
ID3D11Buffer* bufferGpu(Device& d, Buffer* b);           // synced D3D11 mirror of a VkBuffer
void bufferWritten(Buffer* b, VkDeviceSize off, VkDeviceSize len); // driver wrote shadow
void ensureShadow(Device& d, Buffer* b);                 // pull GPU-side writes back into the shadow
ID3D11UnorderedAccessView* bufferUAV(Device& d, Buffer* b, VkDeviceSize off, VkDeviceSize range);
ID3D11ShaderResourceView* viewSRV(Device& d, ImageView* v);
ID3D11RenderTargetView* viewRTV(Device& d, ImageView* v);
ID3D11DepthStencilView* viewDSV(Device& d, ImageView* v);
ID3D11UnorderedAccessView* viewUAV(Device& d, ImageView* v);
Image* createImageObject(Device& d, const VkImageCreateInfo& ci, ID3D11Resource* existing);

// shader.cpp
struct CompiledShader {
    std::vector<uint8_t> bytecode;
    std::string hlsl;
    bool usesVertexInfo = false;
};
bool translateShader(const ShaderModule& mod, const char* entry, VkShaderStageFlagBits stage,
                     const PipelineLayoutData& layout, const VkSpecializationInfo* spec, std::string& hlsl,
                     std::string& err);
bool compileHLSL(const std::string& hlsl, const char* entry, const char* profile, std::vector<uint8_t>& out,
                 std::string& err);
void matchStageInterface(const std::string& vsHlsl, std::string& psHlsl);

// cmd.cpp
void flushGraphics(Device& d);
void executeCommandBuffer(Device& d, CommandBuffer* cb);

// instance.cpp
bool deviceExtensionSupported(const char* name);

// entry table
PFN_vkVoidFunction lookupProc(const char* name, bool deviceOnly);

} // namespace vkon11
