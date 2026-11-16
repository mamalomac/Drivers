// Vulkan loader <-> driver interface and the entry point table.
#include "vkon11.h"
#include <type_traits>

#define INSTANCE_FUNCS(X) \
    X(CreateInstance) X(DestroyInstance) X(EnumeratePhysicalDevices) X(EnumerateInstanceExtensionProperties) \
    X(EnumerateInstanceLayerProperties) X(EnumerateDeviceExtensionProperties) X(EnumerateDeviceLayerProperties) \
    X(GetPhysicalDeviceFeatures) X(GetPhysicalDeviceProperties) X(GetPhysicalDeviceQueueFamilyProperties) \
    X(GetPhysicalDeviceMemoryProperties) X(GetPhysicalDeviceFormatProperties) X(GetPhysicalDeviceImageFormatProperties) \
    X(GetPhysicalDeviceSparseImageFormatProperties) X(GetPhysicalDeviceFeatures2KHR) X(GetPhysicalDeviceProperties2KHR) \
    X(GetPhysicalDeviceFormatProperties2KHR) X(GetPhysicalDeviceImageFormatProperties2KHR) \
    X(GetPhysicalDeviceQueueFamilyProperties2KHR) X(GetPhysicalDeviceMemoryProperties2KHR) \
    X(GetPhysicalDeviceSparseImageFormatProperties2KHR) X(CreateDevice) X(CreateWin32SurfaceKHR) X(DestroySurfaceKHR) \
    X(GetPhysicalDeviceWin32PresentationSupportKHR) X(GetPhysicalDeviceSurfaceSupportKHR) \
    X(GetPhysicalDeviceSurfaceCapabilitiesKHR) X(GetPhysicalDeviceSurfaceFormatsKHR) \
    X(GetPhysicalDeviceSurfacePresentModesKHR)

#define DEVICE_FUNCS(X) \
    X(DestroyDevice) X(GetDeviceQueue) X(DeviceWaitIdle) X(QueueWaitIdle) X(QueueSubmit) X(AllocateMemory) X(FreeMemory) \
    X(MapMemory) X(UnmapMemory) X(FlushMappedMemoryRanges) X(InvalidateMappedMemoryRanges) X(GetDeviceMemoryCommitment) \
    X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) X(BindBufferMemory) X(CreateBufferView) \
    X(DestroyBufferView) X(CreateImage) X(DestroyImage) X(GetImageMemoryRequirements) X(BindImageMemory) \
    X(GetImageSparseMemoryRequirements) X(QueueBindSparse) X(GetImageSubresourceLayout) X(CreateImageView) \
    X(DestroyImageView) X(CreateSampler) X(DestroySampler) X(CreateFence) X(DestroyFence) X(ResetFences) \
    X(GetFenceStatus) X(WaitForFences) X(CreateSemaphore) X(DestroySemaphore) X(CreateEvent) X(DestroyEvent) \
    X(GetEventStatus) X(SetEvent) X(ResetEvent) X(CreateQueryPool) X(DestroyQueryPool) X(GetQueryPoolResults) \
    X(CreateShaderModule) X(DestroyShaderModule) X(CreatePipelineCache) X(DestroyPipelineCache) X(GetPipelineCacheData) \
    X(MergePipelineCaches) X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) X(CreateDescriptorPool) \
    X(DestroyDescriptorPool) X(ResetDescriptorPool) X(AllocateDescriptorSets) X(FreeDescriptorSets) \
    X(UpdateDescriptorSets) X(CreatePipelineLayout) X(DestroyPipelineLayout) X(CreateRenderPass) X(DestroyRenderPass) \
    X(GetRenderAreaGranularity) X(CreateFramebuffer) X(DestroyFramebuffer) X(CreateGraphicsPipelines) \
    X(CreateComputePipelines) X(DestroyPipeline) X(CreateCommandPool) X(DestroyCommandPool) X(ResetCommandPool) \
    X(AllocateCommandBuffers) X(FreeCommandBuffers) X(BeginCommandBuffer) X(EndCommandBuffer) X(ResetCommandBuffer) \
    X(CmdBeginRenderPass) X(CmdNextSubpass) X(CmdEndRenderPass) X(CmdExecuteCommands) X(CmdBindPipeline) \
    X(CmdSetViewport) X(CmdSetScissor) X(CmdSetLineWidth) X(CmdSetDepthBounds) X(CmdSetDepthBias) \
    X(CmdSetBlendConstants) X(CmdSetStencilCompareMask) X(CmdSetStencilWriteMask) X(CmdSetStencilReference) \
    X(CmdBindDescriptorSets) X(CmdBindIndexBuffer) X(CmdBindVertexBuffers) X(CmdPushConstants) X(CmdDraw) \
    X(CmdDrawIndexed) X(CmdDrawIndirect) X(CmdDrawIndexedIndirect) X(CmdDispatch) X(CmdDispatchIndirect) \
    X(CmdCopyBuffer) X(CmdUpdateBuffer) X(CmdFillBuffer) X(CmdCopyBufferToImage) X(CmdCopyImageToBuffer) \
    X(CmdCopyImage) X(CmdBlitImage) X(CmdResolveImage) X(CmdClearColorImage) X(CmdClearDepthStencilImage) \
    X(CmdClearAttachments) X(CmdPipelineBarrier) X(CmdSetEvent) X(CmdResetEvent) X(CmdWaitEvents) X(CmdBeginQuery) \
    X(CmdEndQuery) X(CmdResetQueryPool) X(CmdWriteTimestamp) X(CmdCopyQueryPoolResults) X(CreateSwapchainKHR) \
    X(DestroySwapchainKHR) X(GetSwapchainImagesKHR) X(AcquireNextImageKHR) X(QueuePresentKHR) X(TrimCommandPoolKHR)

namespace vkon11 {

// Declare every entry point with exactly the signature the Vulkan headers use.
#define X(n) extern std::remove_pointer<PFN_vk##n>::type n;
INSTANCE_FUNCS(X)
DEVICE_FUNCS(X)
#undef X

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetInstanceProcAddr(VkInstance, const char* name);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice, const char* name);

struct Entry { const char* name; PFN_vkVoidFunction fn; bool device; };
static const Entry g_entries[] = {
#define X(n) {"vk" #n, (PFN_vkVoidFunction)n, false},
    INSTANCE_FUNCS(X)
#undef X
#define X(n) {"vk" #n, (PFN_vkVoidFunction)n, true},
    DEVICE_FUNCS(X)
#undef X
    {"vkGetInstanceProcAddr", (PFN_vkVoidFunction)GetInstanceProcAddr, false},
    {"vkGetDeviceProcAddr", (PFN_vkVoidFunction)GetDeviceProcAddr, true},
};

PFN_vkVoidFunction lookupProc(const char* name, bool deviceOnly) {
    if (!name) return nullptr;
    for (const Entry& e : g_entries)
        if (!strcmp(e.name, name)) return (deviceOnly && !e.device) ? nullptr : e.fn;
    return nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetInstanceProcAddr(VkInstance, const char* name) {
    return lookupProc(name, false);
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice, const char* name) {
    return lookupProc(name, true);
}

} // namespace vkon11

extern "C" {

VKAPI_ATTR VkResult VKAPI_CALL vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t* version) {
    if (!version) return VK_ERROR_INITIALIZATION_FAILED;
    if (*version > 5) *version = 5;
    return VK_SUCCESS;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char* name) {
    return vkon11::GetInstanceProcAddr(instance, name);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetPhysicalDeviceProcAddr(VkInstance, const char* name) {
    if (!name || strncmp(name, "vkGetPhysicalDevice", 19)) return nullptr;
    return vkon11::lookupProc(name, false);
}

} // extern "C"
