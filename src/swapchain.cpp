// VK_KHR_surface / VK_KHR_win32_surface / VK_KHR_swapchain on top of DXGI.
// Swapchain images are ordinary textures; presenting copies one into the DXGI
// back buffer. That costs a blit per frame but lets Vulkan's "N images that
// stay valid" model work on a D3D11 swap chain.
#include "vkon11.h"

namespace vkon11 {

VKAPI_ATTR VkResult VKAPI_CALL CreateWin32SurfaceKHR(VkInstance, const VkWin32SurfaceCreateInfoKHR* ci,
                                                     const VkAllocationCallbacks*, VkSurfaceKHR* out) {
    VkIcdSurfaceWin32* s = new VkIcdSurfaceWin32();
    s->base.platform = VK_ICD_WSI_PLATFORM_WIN32;
    s->hinstance = ci->hinstance;
    s->hwnd = ci->hwnd;
    *out = to<VkSurfaceKHR>(s);
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL DestroySurfaceKHR(VkInstance, VkSurfaceKHR h, const VkAllocationCallbacks*) {
    delete from<VkIcdSurfaceWin32>(h);
}
VKAPI_ATTR VkBool32 VKAPI_CALL GetPhysicalDeviceWin32PresentationSupportKHR(VkPhysicalDevice, uint32_t) { return VK_TRUE; }
VKAPI_ATTR VkResult VKAPI_CALL GetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice, uint32_t, VkSurfaceKHR, VkBool32* ok) {
    *ok = VK_TRUE;
    return VK_SUCCESS;
}

static VkExtent2D clientExtent(HWND hwnd) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    return {(uint32_t)std::max<LONG>(1, rc.right - rc.left), (uint32_t)std::max<LONG>(1, rc.bottom - rc.top)};
}

VKAPI_ATTR VkResult VKAPI_CALL GetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice, VkSurfaceKHR h,
                                                                       VkSurfaceCapabilitiesKHR* c) {
    VkIcdSurfaceWin32* s = from<VkIcdSurfaceWin32>(h);
    memset(c, 0, sizeof(*c));
    c->minImageCount = 2;
    c->maxImageCount = 8;
    c->currentExtent = c->minImageExtent = c->maxImageExtent = clientExtent(s->hwnd);
    c->maxImageArrayLayers = 1;
    c->supportedTransforms = c->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    c->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR | VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    c->supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                             VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL GetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice, VkSurfaceKHR, uint32_t* count,
                                                                  VkSurfaceFormatKHR* out) {
    static const VkSurfaceFormatKHR fmts[] = {
        {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
        {VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
        {VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
        {VK_FORMAT_R8G8B8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
    };
    const uint32_t n = 4;
    if (!out) { *count = n; return VK_SUCCESS; }
    uint32_t c = std::min(*count, n);
    for (uint32_t i = 0; i < c; i++) out[i] = fmts[i];
    *count = c;
    return c < n ? VK_INCOMPLETE : VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL GetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice, VkSurfaceKHR, uint32_t* count,
                                                                       VkPresentModeKHR* out) {
    static const VkPresentModeKHR modes[] = {VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR};
    const uint32_t n = 2;
    if (!out) { *count = n; return VK_SUCCESS; }
    uint32_t c = std::min(*count, n);
    for (uint32_t i = 0; i < c; i++) out[i] = modes[i];
    *count = c;
    return c < n ? VK_INCOMPLETE : VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateSwapchainKHR(VkDevice h, const VkSwapchainCreateInfoKHR* ci,
                                                  const VkAllocationCallbacks*, VkSwapchainKHR* out) {
    Device& d = *from<Device>(h);
    VkIcdSurfaceWin32* surf = from<VkIcdSurfaceWin32>(ci->surface);
    if (Swapchain* old = from<Swapchain>(ci->oldSwapchain)) old->dxgi.reset(); // retire: one swap chain per window

    auto sc = std::make_unique<Swapchain>();
    sc->hwnd = surf->hwnd;
    sc->extent = ci->imageExtent;
    sc->syncInterval = ci->presentMode == VK_PRESENT_MODE_IMMEDIATE_KHR ? 0 : 1;

    const bool rgba = ci->imageFormat == VK_FORMAT_R8G8B8A8_UNORM || ci->imageFormat == VK_FORMAT_R8G8B8A8_SRGB;
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferDesc.Width = ci->imageExtent.width;
    sd.BufferDesc.Height = ci->imageExtent.height;
    sd.BufferDesc.Format = rgba ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 1;
    sd.OutputWindow = surf->hwnd;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    ComPtr<IDXGIFactory> factory;
    if (FAILED(d.phys->adapter->GetParent(__uuidof(IDXGIFactory), (void**)factory.put()))) return VK_ERROR_INITIALIZATION_FAILED;
    HRESULT hr = factory->CreateSwapChain(d.d3d, &sd, sc->dxgi.put());
    if (FAILED(hr)) {
        VKON_LOG("CreateSwapChain failed hr=0x%08lx", (unsigned long)hr);
        return VK_ERROR_NATIVE_WINDOW_IN_USE_KHR;
    }
    factory->MakeWindowAssociation(surf->hwnd, DXGI_MWA_NO_ALT_ENTER);

    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = ci->imageFormat;
    ici.extent = {ci->imageExtent.width, ci->imageExtent.height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.usage = ci->imageUsage | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    uint32_t n = std::max(2u, std::min(8u, ci->minImageCount));
    for (uint32_t i = 0; i < n; i++) {
        Image* img = createImageObject(d, ici, nullptr);
        if (!img) {
            for (Image* x : sc->images) delete x;
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        sc->images.push_back(img);
    }
    *out = to<VkSwapchainKHR>(sc.release());
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL DestroySwapchainKHR(VkDevice, VkSwapchainKHR h, const VkAllocationCallbacks*) {
    Swapchain* sc = from<Swapchain>(h);
    if (!sc) return;
    for (Image* img : sc->images) delete img;
    delete sc;
}

VKAPI_ATTR VkResult VKAPI_CALL GetSwapchainImagesKHR(VkDevice, VkSwapchainKHR h, uint32_t* count, VkImage* out) {
    Swapchain* sc = from<Swapchain>(h);
    uint32_t n = (uint32_t)sc->images.size();
    if (!out) { *count = n; return VK_SUCCESS; }
    uint32_t c = std::min(*count, n);
    for (uint32_t i = 0; i < c; i++) out[i] = to<VkImage>(sc->images[i]);
    *count = c;
    return c < n ? VK_INCOMPLETE : VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL AcquireNextImageKHR(VkDevice, VkSwapchainKHR h, uint64_t, VkSemaphore, VkFence fence,
                                                   uint32_t* index) {
    Swapchain* sc = from<Swapchain>(h);
    *index = sc->next;
    sc->next = (sc->next + 1) % (uint32_t)sc->images.size();
    if (fence) from<Fence>(fence)->signaled = true;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL QueuePresentKHR(VkQueue q, const VkPresentInfoKHR* pi) {
    Device& d = *from<Queue>(q)->device;
    std::lock_guard<std::mutex> lk(d.phys->ctxMutex);
    VkResult result = VK_SUCCESS;
    for (uint32_t i = 0; i < pi->swapchainCount; i++) {
        Swapchain* sc = from<Swapchain>(pi->pSwapchains[i]);
        VkResult r = VK_SUCCESS;
        if (!sc->dxgi || pi->pImageIndices[i] >= sc->images.size()) r = VK_ERROR_OUT_OF_DATE_KHR;
        else {
            ComPtr<ID3D11Texture2D> back;
            if (SUCCEEDED(sc->dxgi->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)back.put()))) {
                d.ctx->CopyResource(back.get(), sc->images[pi->pImageIndices[i]]->tex.get());
                HRESULT hr = sc->dxgi->Present(sc->syncInterval, 0);
                if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) r = VK_ERROR_DEVICE_LOST;
            } else r = VK_ERROR_SURFACE_LOST_KHR;
            VkExtent2D now = clientExtent(sc->hwnd);
            if (r == VK_SUCCESS && (now.width != sc->extent.width || now.height != sc->extent.height)) r = VK_SUBOPTIMAL_KHR;
        }
        if (pi->pResults) pi->pResults[i] = r;
        if (r != VK_SUCCESS && (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR)) result = r;
    }
    return result;
}

} // namespace vkon11
