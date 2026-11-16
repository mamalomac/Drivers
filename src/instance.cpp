// Instance, physical device enumeration, capability reporting.
#include "vkon11.h"
#include <cstdarg>
#include <cmath>

namespace vkon11 {

void logf(const char* fmt, ...) {
    static int mode = -1;
    static FILE* file = nullptr;
    if (mode < 0) {
        char buf[512];
        DWORD n = GetEnvironmentVariableA("VKON11_LOG", buf, sizeof(buf));
        if (n == 0 || n >= sizeof(buf) || !strcmp(buf, "0")) mode = 0;
        else if (!strcmp(buf, "1")) { mode = 1; file = stderr; }
        else { mode = 1; file = fopen(buf, "a"); if (!file) file = stderr; }
    }
    if (!mode) return;
    va_list ap;
    va_start(ap, fmt);
    fputs("[vkon11] ", file);
    vfprintf(file, fmt, ap);
    fputc('\n', file);
    fflush(file);
    va_end(ap);
}

static const VkExtensionProperties g_instanceExts[] = {
    {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_SURFACE_SPEC_VERSION},
    {VK_KHR_WIN32_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_SPEC_VERSION},
    {VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_SPEC_VERSION},
};
static const VkExtensionProperties g_deviceExts[] = {
    {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_SWAPCHAIN_SPEC_VERSION},
    {VK_KHR_MAINTENANCE_1_EXTENSION_NAME, VK_KHR_MAINTENANCE_1_SPEC_VERSION},
};

bool deviceExtensionSupported(const char* name) {
    for (const auto& e : g_deviceExts)
        if (!strcmp(e.extensionName, name)) return true;
    return false;
}

template <class T, size_t N>
static VkResult copyOut(const T (&src)[N], uint32_t* count, T* dst) {
    if (!dst) { *count = (uint32_t)N; return VK_SUCCESS; }
    uint32_t n = std::min<uint32_t>(*count, (uint32_t)N);
    for (uint32_t i = 0; i < n; i++) dst[i] = src[i];
    *count = n;
    return n < N ? VK_INCOMPLETE : VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL EnumerateInstanceExtensionProperties(const char* layer, uint32_t* count,
                                                                    VkExtensionProperties* props) {
    if (layer) return VK_ERROR_LAYER_NOT_PRESENT;
    return copyOut(g_instanceExts, count, props);
}
VKAPI_ATTR VkResult VKAPI_CALL EnumerateInstanceLayerProperties(uint32_t* count, VkLayerProperties*) {
    *count = 0;
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL EnumerateDeviceExtensionProperties(VkPhysicalDevice, const char* layer,
                                                                  uint32_t* count, VkExtensionProperties* props) {
    if (layer) return VK_ERROR_LAYER_NOT_PRESENT;
    return copyOut(g_deviceExts, count, props);
}
VKAPI_ATTR VkResult VKAPI_CALL EnumerateDeviceLayerProperties(VkPhysicalDevice, uint32_t* count, VkLayerProperties*) {
    *count = 0;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo* ci, const VkAllocationCallbacks*,
                                              VkInstance* out) {
    for (uint32_t i = 0; i < ci->enabledExtensionCount; i++) {
        bool ok = false;
        for (const auto& e : g_instanceExts)
            if (!strcmp(e.extensionName, ci->ppEnabledExtensionNames[i])) ok = true;
        if (!ok) return VK_ERROR_EXTENSION_NOT_PRESENT;
    }
    Instance* inst = new Instance();
    set_loader_magic_value(inst);
    *out = to<VkInstance>(inst);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL DestroyInstance(VkInstance h, const VkAllocationCallbacks*) {
    delete from<Instance>(h);
}

static void enumerateAdapters(Instance* inst) {
    if (inst->enumerated) return;
    inst->enumerated = true;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)factory.put()))) {
        VKON_LOG("CreateDXGIFactory1 failed");
        return;
    }
    char only[64] = "";
    GetEnvironmentVariableA("VKON11_ADAPTER", only, sizeof(only)); // optional adapter index filter
    for (UINT i = 0;; i++) {
        ComPtr<IDXGIAdapter> adapter;
        if (factory->EnumAdapters(i, adapter.put()) == DXGI_ERROR_NOT_FOUND) break;
        if (only[0] && (UINT)atoi(only) != i) continue;
        auto pd = std::make_unique<PhysicalDevice>();
        adapter->GetDesc(&pd->desc);
        static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        UINT flags = 0;
        char dbg[8];
        if (GetEnvironmentVariableA("VKON11_D3D_DEBUG", dbg, sizeof(dbg))) flags |= D3D11_CREATE_DEVICE_DEBUG;
        HRESULT hr = D3D11CreateDevice(adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels, 2,
                                       D3D11_SDK_VERSION, pd->d3d.put(), &pd->featureLevel, pd->ctx.put());
        if (hr == E_INVALIDARG)
            hr = D3D11CreateDevice(adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels + 1, 1,
                                   D3D11_SDK_VERSION, pd->d3d.put(), &pd->featureLevel, pd->ctx.put());
        if (FAILED(hr)) {
            VKON_LOG("adapter %u: no D3D11 feature level 11_0 device (hr=0x%08lx)", i, (unsigned long)hr);
            continue;
        }
        set_loader_magic_value(pd.get());
        pd->instance = inst;
        pd->adapter = adapter;
        VKON_LOG("adapter %u: %ls vendor=%04x device=%04x vram=%lluMB fl=%x", i, pd->desc.Description,
                 pd->desc.VendorId, pd->desc.DeviceId,
                 (unsigned long long)(pd->desc.DedicatedVideoMemory >> 20), (unsigned)pd->featureLevel);
        inst->physicalDevices.push_back(std::move(pd));
    }
}

VKAPI_ATTR VkResult VKAPI_CALL EnumeratePhysicalDevices(VkInstance h, uint32_t* count, VkPhysicalDevice* out) {
    Instance* inst = from<Instance>(h);
    enumerateAdapters(inst);
    uint32_t n = (uint32_t)inst->physicalDevices.size();
    if (!out) { *count = n; return VK_SUCCESS; }
    uint32_t c = std::min(*count, n);
    for (uint32_t i = 0; i < c; i++) out[i] = to<VkPhysicalDevice>(inst->physicalDevices[i].get());
    *count = c;
    return c < n ? VK_INCOMPLETE : VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceFeatures(VkPhysicalDevice, VkPhysicalDeviceFeatures* f) {
    memset(f, 0, sizeof(*f));
    f->robustBufferAccess = VK_TRUE;
    f->fullDrawIndexUint32 = VK_TRUE;
    f->imageCubeArray = VK_TRUE;
    f->independentBlend = VK_TRUE;
    f->dualSrcBlend = VK_TRUE;
    f->multiDrawIndirect = VK_TRUE;
    f->drawIndirectFirstInstance = VK_TRUE;
    f->depthClamp = VK_TRUE;
    f->depthBiasClamp = VK_TRUE;
    f->fillModeNonSolid = VK_TRUE;
    f->samplerAnisotropy = VK_TRUE;
    f->textureCompressionBC = VK_TRUE;
    f->occlusionQueryPrecise = VK_TRUE;
    // Only features with an implementation behind them are reported. Not
    // available yet: geometryShader and tessellationShader (the SPIR-V -> HLSL
    // path has no GS/HS/DS support), fragment-stage storage writes, clip/cull
    // distances, multiViewport, dynamic resource-array indexing, logicOp,
    // wideLines, largePoints, sparse resources.
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceProperties(VkPhysicalDevice h, VkPhysicalDeviceProperties* p) {
    PhysicalDevice* pd = from<PhysicalDevice>(h);
    memset(p, 0, sizeof(*p));
    p->apiVersion = VK_MAKE_API_VERSION(0, 1, 0, VK_HEADER_VERSION);
    p->driverVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
    p->vendorID = pd->desc.VendorId;
    p->deviceID = pd->desc.DeviceId;
    bool software = pd->desc.VendorId == 0x1414 && pd->desc.DeviceId == 0x8c; // Microsoft Basic Render
    p->deviceType = software ? VK_PHYSICAL_DEVICE_TYPE_CPU
                    : pd->desc.DedicatedVideoMemory ? VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU
                                                    : VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
    char name[128];
    WideCharToMultiByte(CP_UTF8, 0, pd->desc.Description, -1, name, sizeof(name), nullptr, nullptr);
    name[sizeof(name) - 1] = 0;
    snprintf(p->deviceName, sizeof(p->deviceName), "%s (VKOn11)", name);
    memcpy(p->pipelineCacheUUID, "VKOn11-D3D11-001", 16);

    VkPhysicalDeviceLimits& l = p->limits;
    l.maxImageDimension1D = 16384;
    l.maxImageDimension2D = 16384;
    l.maxImageDimension3D = 2048;
    l.maxImageDimensionCube = 16384;
    l.maxImageArrayLayers = 2048;
    l.maxTexelBufferElements = 1 << 27;
    l.maxUniformBufferRange = 65536;
    l.maxStorageBufferRange = 1u << 27;
    l.maxPushConstantsSize = 128;
    l.maxMemoryAllocationCount = 4096;
    l.maxSamplerAllocationCount = 4000;
    l.bufferImageGranularity = 1;
    l.sparseAddressSpaceSize = 0;
    l.maxBoundDescriptorSets = 8;
    l.maxPerStageDescriptorSamplers = MAX_SAMPLER;
    l.maxPerStageDescriptorUniformBuffers = MAX_USER_CBV;
    l.maxPerStageDescriptorStorageBuffers = 4;
    l.maxPerStageDescriptorSampledImages = MAX_SRV;
    l.maxPerStageDescriptorStorageImages = 4;
    l.maxPerStageDescriptorInputAttachments = 4;
    l.maxPerStageResources = MAX_USER_CBV + MAX_SRV + MAX_UAV;
    l.maxDescriptorSetSamplers = MAX_SAMPLER * 2;
    l.maxDescriptorSetUniformBuffers = MAX_USER_CBV * 2;
    l.maxDescriptorSetUniformBuffersDynamic = 8;
    l.maxDescriptorSetStorageBuffers = 8;
    l.maxDescriptorSetStorageBuffersDynamic = 4;
    l.maxDescriptorSetSampledImages = MAX_SRV * 2;
    l.maxDescriptorSetStorageImages = 8;
    l.maxDescriptorSetInputAttachments = 4;
    l.maxVertexInputAttributes = 32;
    l.maxVertexInputBindings = 32;
    l.maxVertexInputAttributeOffset = 2047;
    l.maxVertexInputBindingStride = 2048;
    l.maxVertexOutputComponents = 124;
    l.maxFragmentInputComponents = 124;
    l.maxFragmentOutputAttachments = 8;
    l.maxFragmentDualSrcAttachments = 1;
    l.maxFragmentCombinedOutputResources = 8 + MAX_UAV;
    l.maxComputeSharedMemorySize = 32768;
    l.maxComputeWorkGroupCount[0] = l.maxComputeWorkGroupCount[1] = l.maxComputeWorkGroupCount[2] = 65535;
    l.maxComputeWorkGroupInvocations = 1024;
    l.maxComputeWorkGroupSize[0] = l.maxComputeWorkGroupSize[1] = 1024;
    l.maxComputeWorkGroupSize[2] = 64;
    l.subPixelPrecisionBits = 8;
    l.subTexelPrecisionBits = 8;
    l.mipmapPrecisionBits = 8;
    l.maxDrawIndexedIndexValue = 0xffffffffu;
    l.maxDrawIndirectCount = 0xffffffffu;
    l.maxSamplerLodBias = 15.99f;
    l.maxSamplerAnisotropy = 16.0f;
    l.maxViewports = 1;
    l.maxViewportDimensions[0] = l.maxViewportDimensions[1] = 16384;
    l.viewportBoundsRange[0] = -32768.0f;
    l.viewportBoundsRange[1] = 32767.0f;
    l.viewportSubPixelBits = 8;
    l.minMemoryMapAlignment = 4096;
    l.minTexelBufferOffsetAlignment = 16;
    l.minUniformBufferOffsetAlignment = 16;
    l.minStorageBufferOffsetAlignment = 16;
    l.minTexelOffset = -8;
    l.maxTexelOffset = 7;
    l.minTexelGatherOffset = -32;
    l.maxTexelGatherOffset = 31;
    l.minInterpolationOffset = -0.5f;
    l.maxInterpolationOffset = 0.4375f;
    l.subPixelInterpolationOffsetBits = 4;
    l.maxFramebufferWidth = l.maxFramebufferHeight = 16384;
    l.maxFramebufferLayers = 2048;
    VkSampleCountFlags sc = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_2_BIT | VK_SAMPLE_COUNT_4_BIT | VK_SAMPLE_COUNT_8_BIT;
    l.framebufferColorSampleCounts = l.framebufferDepthSampleCounts = l.framebufferStencilSampleCounts = sc;
    l.framebufferNoAttachmentsSampleCounts = VK_SAMPLE_COUNT_1_BIT;
    l.maxColorAttachments = 8;
    l.sampledImageColorSampleCounts = l.sampledImageDepthSampleCounts = l.sampledImageStencilSampleCounts = sc;
    l.sampledImageIntegerSampleCounts = VK_SAMPLE_COUNT_1_BIT;
    l.storageImageSampleCounts = VK_SAMPLE_COUNT_1_BIT;
    l.maxSampleMaskWords = 1;
    l.timestampComputeAndGraphics = VK_FALSE;
    l.timestampPeriod = 1.0f;
    l.maxClipDistances = 0;
    l.maxCullDistances = 0;
    l.maxCombinedClipAndCullDistances = 0;
    l.discreteQueuePriorities = 2;
    l.pointSizeRange[0] = l.pointSizeRange[1] = 1.0f;
    l.lineWidthRange[0] = l.lineWidthRange[1] = 1.0f;
    l.pointSizeGranularity = 0.0f;
    l.lineWidthGranularity = 0.0f;
    l.strictLines = VK_TRUE;
    l.standardSampleLocations = VK_TRUE;
    l.optimalBufferCopyOffsetAlignment = 1;
    l.optimalBufferCopyRowPitchAlignment = 1;
    l.nonCoherentAtomSize = 1;
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice, uint32_t* count,
                                                                  VkQueueFamilyProperties* props) {
    if (!props) { *count = 1; return; }
    if (*count == 0) return;
    props[0].queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
    props[0].queueCount = 1;
    props[0].timestampValidBits = 0;
    props[0].minImageTransferGranularity = {1, 1, 1};
    *count = 1;
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceMemoryProperties(VkPhysicalDevice h, VkPhysicalDeviceMemoryProperties* p) {
    PhysicalDevice* pd = from<PhysicalDevice>(h);
    memset(p, 0, sizeof(*p));
    VkDeviceSize vram = pd->desc.DedicatedVideoMemory ? pd->desc.DedicatedVideoMemory : (VkDeviceSize)1 << 30;
    VkDeviceSize shared = pd->desc.SharedSystemMemory ? pd->desc.SharedSystemMemory : (VkDeviceSize)2 << 30;
    p->memoryHeapCount = 2;
    p->memoryHeaps[0] = {vram, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT};
    p->memoryHeaps[1] = {shared, 0};
    p->memoryTypeCount = 2;
    p->memoryTypes[0] = {VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0};
    p->memoryTypes[1] = {VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                             VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 1};
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceFormatProperties(VkPhysicalDevice h, VkFormat format,
                                                             VkFormatProperties* p) {
    PhysicalDevice* pd = from<PhysicalDevice>(h);
    memset(p, 0, sizeof(*p));
    const FormatInfo* fi = findFormat(format);
    if (!fi) return;
    UINT s = 0, ssrv = 0;
    pd->d3d->CheckFormatSupport(fi->dxgi, &s);
    pd->d3d->CheckFormatSupport(fi->srv, &ssrv);
    if (s & D3D11_FORMAT_SUPPORT_IA_VERTEX_BUFFER) p->bufferFeatures |= VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT;
    if (fi->flags & FMT_VERTEX_ONLY) return;
    VkFormatFeatureFlags f = 0;
    if ((ssrv & D3D11_FORMAT_SUPPORT_TEXTURE2D) && (ssrv & (D3D11_FORMAT_SUPPORT_SHADER_LOAD | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE))) {
        f |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT_KHR | VK_FORMAT_FEATURE_TRANSFER_DST_BIT_KHR;
        if (!(fi->flags & FMT_DEPTH)) f |= VK_FORMAT_FEATURE_BLIT_SRC_BIT;
        if ((ssrv & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE) && !(fi->flags & (FMT_INT | FMT_DEPTH)))
            f |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    }
    if (fi->flags & FMT_DEPTH) {
        if (s & D3D11_FORMAT_SUPPORT_DEPTH_STENCIL) f |= VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
    } else if (!(fi->flags & FMT_COMPRESSED)) {
        if (s & D3D11_FORMAT_SUPPORT_RENDER_TARGET) {
            f |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
            if (!(fi->flags & FMT_INT)) f |= VK_FORMAT_FEATURE_BLIT_DST_BIT;
        }
        if (s & D3D11_FORMAT_SUPPORT_BLENDABLE) f |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
        if (s & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW) f |= VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
    }
    p->optimalTilingFeatures = f;
    p->linearTilingFeatures = 0; // images are never host-mappable in this driver
}

VKAPI_ATTR VkResult VKAPI_CALL GetPhysicalDeviceImageFormatProperties(VkPhysicalDevice h, VkFormat format,
                                                                      VkImageType type, VkImageTiling tiling,
                                                                      VkImageUsageFlags usage, VkImageCreateFlags,
                                                                      VkImageFormatProperties* p) {
    PhysicalDevice* pd = from<PhysicalDevice>(h);
    memset(p, 0, sizeof(*p));
    VkFormatProperties fp;
    GetPhysicalDeviceFormatProperties(h, format, &fp);
    const FormatInfo* fi = findFormat(format);
    if (!fi || tiling != VK_IMAGE_TILING_OPTIMAL || !fp.optimalTilingFeatures) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    VkFormatFeatureFlags f = fp.optimalTilingFeatures;
    if ((usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) && !(f & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    if ((usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) && !(f & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    if ((usage & VK_IMAGE_USAGE_SAMPLED_BIT) && !(f & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    if ((usage & VK_IMAGE_USAGE_STORAGE_BIT) && !(f & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    uint32_t maxDim = type == VK_IMAGE_TYPE_3D ? 2048 : 16384;
    p->maxExtent = {maxDim, type == VK_IMAGE_TYPE_1D ? 1 : maxDim, type == VK_IMAGE_TYPE_3D ? maxDim : 1};
    p->maxMipLevels = type == VK_IMAGE_TYPE_3D ? 12 : 15;
    p->maxArrayLayers = type == VK_IMAGE_TYPE_3D ? 1 : 2048;
    p->sampleCounts = VK_SAMPLE_COUNT_1_BIT;
    bool renderable = f & (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT);
    if (type == VK_IMAGE_TYPE_2D && renderable && !(fi->flags & FMT_COMPRESSED)) {
        for (UINT n = 2; n <= 8; n *= 2) {
            UINT q = 0;
            if (SUCCEEDED(pd->d3d->CheckMultisampleQualityLevels(fi->dxgi, n, &q)) && q > 0) p->sampleCounts |= n;
        }
    }
    p->maxResourceSize = (VkDeviceSize)1 << 31;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceSparseImageFormatProperties(VkPhysicalDevice, VkFormat, VkImageType,
                                                                        VkSampleCountFlagBits, VkImageUsageFlags,
                                                                        VkImageTiling, uint32_t* count,
                                                                        VkSparseImageFormatProperties*) {
    *count = 0;
}

// ---- VK_KHR_get_physical_device_properties2 --------------------------------
VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceFeatures2KHR(VkPhysicalDevice h, VkPhysicalDeviceFeatures2* f) {
    GetPhysicalDeviceFeatures(h, &f->features);
}
VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceProperties2KHR(VkPhysicalDevice h, VkPhysicalDeviceProperties2* p) {
    GetPhysicalDeviceProperties(h, &p->properties);
}
VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceFormatProperties2KHR(VkPhysicalDevice h, VkFormat fmt,
                                                                 VkFormatProperties2* p) {
    GetPhysicalDeviceFormatProperties(h, fmt, &p->formatProperties);
}
VKAPI_ATTR VkResult VKAPI_CALL GetPhysicalDeviceImageFormatProperties2KHR(
    VkPhysicalDevice h, const VkPhysicalDeviceImageFormatInfo2* i, VkImageFormatProperties2* p) {
    return GetPhysicalDeviceImageFormatProperties(h, i->format, i->type, i->tiling, i->usage, i->flags,
                                                  &p->imageFormatProperties);
}
VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceQueueFamilyProperties2KHR(VkPhysicalDevice h, uint32_t* count,
                                                                      VkQueueFamilyProperties2* p) {
    if (!p) { *count = 1; return; }
    if (*count == 0) return;
    GetPhysicalDeviceQueueFamilyProperties(h, count, &p->queueFamilyProperties);
}
VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceMemoryProperties2KHR(VkPhysicalDevice h,
                                                                 VkPhysicalDeviceMemoryProperties2* p) {
    GetPhysicalDeviceMemoryProperties(h, &p->memoryProperties);
}
VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceSparseImageFormatProperties2KHR(
    VkPhysicalDevice, const VkPhysicalDeviceSparseImageFormatInfo2*, uint32_t* count, VkSparseImageFormatProperties2*) {
    *count = 0;
}

} // namespace vkon11
