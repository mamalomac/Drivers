// Device, memory, buffers, images, views, samplers, sync objects, queries.
#include "vkon11.h"

namespace vkon11 {

static const VkDeviceSize PAGE = 4096;

// ---------------------------------------------------------------------------
// Device
VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice h, const VkDeviceCreateInfo* ci,
                                            const VkAllocationCallbacks*, VkDevice* out) {
    PhysicalDevice* pd = from<PhysicalDevice>(h);
    for (uint32_t i = 0; i < ci->enabledExtensionCount; i++)
        if (!deviceExtensionSupported(ci->ppEnabledExtensionNames[i])) return VK_ERROR_EXTENSION_NOT_PRESENT;
    Device* d = new Device();
    set_loader_magic_value(d);
    set_loader_magic_value(&d->queue);
    d->queue.device = d;
    d->phys = pd;
    d->d3d = pd->d3d.get();
    d->ctx = pd->ctx.get();
    d->ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)d->ctx1.put());
    if (ci->pEnabledFeatures) d->enabled = *ci->pEnabledFeatures;
    *out = to<VkDevice>(d);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL DestroyDevice(VkDevice h, const VkAllocationCallbacks*) {
    Device* d = from<Device>(h);
    if (!d) return;
    {
        std::lock_guard<std::mutex> lk(d->phys->ctxMutex);
        d->ctx->ClearState();
        d->ctx->Flush();
    }
    delete d;
}

VKAPI_ATTR void VKAPI_CALL GetDeviceQueue(VkDevice h, uint32_t, uint32_t, VkQueue* out) {
    *out = to<VkQueue>(&from<Device>(h)->queue);
}

VKAPI_ATTR VkResult VKAPI_CALL DeviceWaitIdle(VkDevice h) {
    Device* d = from<Device>(h);
    std::lock_guard<std::mutex> lk(d->phys->ctxMutex);
    d->ctx->Flush();
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL QueueWaitIdle(VkQueue q) {
    return DeviceWaitIdle(to<VkDevice>(from<Queue>(q)->device));
}

VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit(VkQueue q, uint32_t count, const VkSubmitInfo* submits, VkFence fence) {
    Device& d = *from<Queue>(q)->device;
    std::lock_guard<std::mutex> lk(d.phys->ctxMutex);
    for (uint32_t i = 0; i < count; i++)
        for (uint32_t j = 0; j < submits[i].commandBufferCount; j++)
            executeCommandBuffer(d, from<CommandBuffer>(submits[i].pCommandBuffers[j]));
    d.endSubmit();
    d.ctx->Flush();
    if (fence) from<Fence>(fence)->signaled = true;
    return VK_SUCCESS;
}

ID3D11Buffer* Device::uniformCB(Buffer* b, VkDeviceSize off, VkDeviceSize range) {
    if (!b || !b->mem) return nullptr;
    if (off >= b->ci.size) return nullptr;
    if (range == VK_WHOLE_SIZE || off + range > b->ci.size) range = b->ci.size - off;
    range = std::min<VkDeviceSize>(range, 65536);
    ensureShadow(*this, b);
    CbKey key{b, off, range, b->version};
    auto it = cbCache.find(key);
    if (it != cbCache.end()) return it->second;
    int bucket = 8;
    while (((VkDeviceSize)1 << bucket) < range) bucket++;
    ComPtr<ID3D11Buffer> cb;
    if (!cbFree[bucket].empty()) {
        cb = std::move(cbFree[bucket].back());
        cbFree[bucket].pop_back();
    } else {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = 1u << bucket;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(d3d->CreateBuffer(&bd, nullptr, cb.put()))) return nullptr;
    }
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ctx->Map(cb.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, b->data() + off, (size_t)range);
        ctx->Unmap(cb.get(), 0);
    }
    ID3D11Buffer* raw = cb.get();
    cbUsed.emplace_back(bucket, std::move(cb));
    cbCache[key] = raw;
    return raw;
}

void Device::endSubmit() {
    std::vector<Buffer*> written;
    written.swap(gpuWritten);
    for (Buffer* b : written) ensureShadow(*this, b);
    pendingUavBuffers.clear();
    tempUavs.clear();
    for (auto& u : cbUsed) cbFree[u.first].push_back(std::move(u.second));
    cbUsed.clear();
    cbCache.clear();
    submitId++;
}

// ---------------------------------------------------------------------------
// Memory
void Memory::commit(VkDeviceSize off, VkDeviceSize len) {
    if (committed || !base || !len) return;
    VkDeviceSize a = off & ~(PAGE - 1), e = std::min(size, (off + len + PAGE - 1) & ~(PAGE - 1));
    VirtualAlloc(base + a, (SIZE_T)(e - a), MEM_COMMIT, PAGE_READWRITE);
}

void Memory::scan(VkDeviceSize off, VkDeviceSize len) {
    if (!watch || !len) return;
    VkDeviceSize a = off & ~(PAGE - 1), e = (off + len + PAGE - 1) & ~(PAGE - 1);
    uint32_t next = epoch + 1;
    bool any = false;
    for (;;) {
        void* addrs[1024];
        ULONG_PTR n = 1024;
        DWORD gran = 0;
        if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, base + a, (SIZE_T)(e - a), addrs, &n, &gran) != 0) {
            // Tracking unavailable: fall back to "everything is always dirty".
            watch = false;
            return;
        }
        for (ULONG_PTR i = 0; i < n; i++) {
            size_t page = (size_t)(((uint8_t*)addrs[i] - base) / PAGE);
            if (page < pageEpoch.size()) pageEpoch[page] = next;
        }
        any |= n > 0;
        if (n < 1024) break;
    }
    if (any) epoch = next;
}

void Memory::markDirty(VkDeviceSize off, VkDeviceSize len) {
    if (!len || pageEpoch.empty()) return;
    ++epoch;
    size_t a = (size_t)(off / PAGE), e = (size_t)std::min<VkDeviceSize>((off + len - 1) / PAGE, pageEpoch.size() - 1);
    for (size_t p = a; p <= e; p++) pageEpoch[p] = epoch;
}

VKAPI_ATTR VkResult VKAPI_CALL AllocateMemory(VkDevice, const VkMemoryAllocateInfo* ai, const VkAllocationCallbacks*,
                                              VkDeviceMemory* out) {
    Memory* m = new Memory();
    m->size = (ai->allocationSize + PAGE - 1) & ~(PAGE - 1);
    m->hostVisible = ai->memoryTypeIndex == 1;
    // Host-visible memory is committed up front (it can be mapped at any time);
    // device-local memory only gets pages when a buffer is bound to it.
    DWORD type = MEM_RESERVE | (m->hostVisible ? MEM_COMMIT : 0);
    m->base = (uint8_t*)VirtualAlloc(nullptr, (SIZE_T)m->size, type | MEM_WRITE_WATCH, PAGE_READWRITE);
    m->watch = m->base != nullptr;
    if (!m->base) m->base = (uint8_t*)VirtualAlloc(nullptr, (SIZE_T)m->size, type, PAGE_READWRITE);
    if (!m->base) { delete m; return VK_ERROR_OUT_OF_HOST_MEMORY; }
    m->committed = m->hostVisible;
    m->pageEpoch.assign((size_t)(m->size / PAGE), 0);
    *out = to<VkDeviceMemory>(m);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL FreeMemory(VkDevice, VkDeviceMemory h, const VkAllocationCallbacks*) {
    Memory* m = from<Memory>(h);
    if (!m) return;
    VirtualFree(m->base, 0, MEM_RELEASE);
    delete m;
}

VKAPI_ATTR VkResult VKAPI_CALL MapMemory(VkDevice, VkDeviceMemory h, VkDeviceSize offset, VkDeviceSize,
                                         VkMemoryMapFlags, void** data) {
    Memory* m = from<Memory>(h);
    if (!m->hostVisible) return VK_ERROR_MEMORY_MAP_FAILED;
    *data = m->base + offset;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL UnmapMemory(VkDevice, VkDeviceMemory) {}
VKAPI_ATTR VkResult VKAPI_CALL FlushMappedMemoryRanges(VkDevice, uint32_t, const VkMappedMemoryRange*) { return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL InvalidateMappedMemoryRanges(VkDevice, uint32_t, const VkMappedMemoryRange*) { return VK_SUCCESS; }
VKAPI_ATTR void VKAPI_CALL GetDeviceMemoryCommitment(VkDevice, VkDeviceMemory h, VkDeviceSize* out) {
    *out = from<Memory>(h)->size;
}

// ---------------------------------------------------------------------------
// Buffers
VKAPI_ATTR VkResult VKAPI_CALL CreateBuffer(VkDevice, const VkBufferCreateInfo* ci, const VkAllocationCallbacks*,
                                            VkBuffer* out) {
    Buffer* b = new Buffer();
    b->ci = *ci;
    b->ci.pNext = nullptr;
    b->ci.pQueueFamilyIndices = nullptr;
    *out = to<VkBuffer>(b);
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL DestroyBuffer(VkDevice, VkBuffer h, const VkAllocationCallbacks*) { delete from<Buffer>(h); }

VKAPI_ATTR void VKAPI_CALL GetBufferMemoryRequirements(VkDevice, VkBuffer h, VkMemoryRequirements* r) {
    Buffer* b = from<Buffer>(h);
    r->size = (b->ci.size + 63) & ~(VkDeviceSize)63;
    r->alignment = 64;
    r->memoryTypeBits = 3;
}

VKAPI_ATTR VkResult VKAPI_CALL BindBufferMemory(VkDevice, VkBuffer h, VkDeviceMemory mh, VkDeviceSize offset) {
    Buffer* b = from<Buffer>(h);
    b->mem = from<Memory>(mh);
    b->memOffset = offset;
    b->mem->commit(offset, b->ci.size);
    return VK_SUCCESS;
}

ID3D11Buffer* bufferGpu(Device& d, Buffer* b) {
    if (!b || !b->mem) return nullptr;
    if (b->d3d && b->lastSyncSubmit == d.submitId) return b->d3d.get();
    Memory* m = b->mem;
    m->scan(b->memOffset, b->ci.size);
    if (!b->d3d) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = (UINT)b->ci.size;
        bd.Usage = D3D11_USAGE_DEFAULT;
        if (b->ci.usage & VK_BUFFER_USAGE_INDEX_BUFFER_BIT) bd.BindFlags |= D3D11_BIND_INDEX_BUFFER;
        if (b->ci.usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) {
            bd.ByteWidth = (bd.ByteWidth + 3) & ~3u; // raw views address 32-bit words
            bd.BindFlags |= D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
            bd.MiscFlags |= D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        }
        if ((b->ci.usage & VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) || !bd.BindFlags) bd.BindFlags |= D3D11_BIND_VERTEX_BUFFER;
        std::vector<uint8_t> padded;
        const void* src = b->data();
        if (bd.ByteWidth != b->ci.size) { // never read past the end of the shadow
            padded.assign(bd.ByteWidth, 0);
            memcpy(padded.data(), b->data(), (size_t)b->ci.size);
            src = padded.data();
        }
        D3D11_SUBRESOURCE_DATA init{src, 0, 0};
        if (FAILED(d.d3d->CreateBuffer(&bd, &init, b->d3d.put()))) {
            VKON_LOG("CreateBuffer(%u bytes) failed", bd.ByteWidth);
            return nullptr;
        }
    } else if (!m->watch) {
        d.ctx->UpdateSubresource(b->d3d.get(), 0, nullptr, b->data(), 0, 0);
    } else {
        VkDeviceSize begin = b->memOffset, end = b->memOffset + b->ci.size;
        size_t first = (size_t)(begin / PAGE), last = (size_t)((end - 1) / PAGE);
        for (size_t p = first; p <= last;) {
            if (m->pageEpoch[p] <= b->syncedEpoch) { p++; continue; }
            size_t q = p;
            while (q <= last && m->pageEpoch[q] > b->syncedEpoch) q++;
            VkDeviceSize a = std::max<VkDeviceSize>(p * PAGE, begin), e = std::min<VkDeviceSize>(q * PAGE, end);
            D3D11_BOX box{(UINT)(a - begin), 0, 0, (UINT)(e - begin), 1, 1};
            d.ctx->UpdateSubresource(b->d3d.get(), 0, &box, m->base + a, 0, 0);
            p = q;
        }
    }
    b->syncedEpoch = m->epoch;
    b->lastSyncSubmit = d.submitId;
    return b->d3d.get();
}

// A compute shader wrote the D3D11 buffer: copy it back so the shadow (and any
// host mapping of it) is current again.
void ensureShadow(Device& d, Buffer* b) {
    if (!b || !b->gpuWritten || !b->d3d || !b->mem) return;
    b->gpuWritten = false;
    D3D11_BUFFER_DESC bd;
    b->d3d->GetDesc(&bd);
    bd.Usage = D3D11_USAGE_STAGING;
    bd.BindFlags = 0;
    bd.MiscFlags = 0;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Buffer> staging;
    if (FAILED(d.d3d->CreateBuffer(&bd, nullptr, staging.put()))) return;
    d.ctx->CopyResource(staging.get(), b->d3d.get());
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(d.ctx->Map(staging.get(), 0, D3D11_MAP_READ, 0, &m))) {
        memcpy(b->data(), m.pData, (size_t)b->ci.size);
        d.ctx->Unmap(staging.get(), 0);
    }
    b->version++;
}

ID3D11UnorderedAccessView* bufferUAV(Device& d, Buffer* b, VkDeviceSize off, VkDeviceSize range) {
    ID3D11Buffer* buf = bufferGpu(d, b);
    if (!buf || !(b->ci.usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) || off >= b->ci.size) return nullptr;
    if (range == VK_WHOLE_SIZE || off + range > b->ci.size) range = b->ci.size - off;
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = DXGI_FORMAT_R32_TYPELESS;
    ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    ud.Buffer.FirstElement = (UINT)(off / 4);
    ud.Buffer.NumElements = (UINT)((range + 3) / 4);
    ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    ComPtr<ID3D11UnorderedAccessView> uav;
    if (FAILED(d.d3d->CreateUnorderedAccessView(buf, &ud, uav.put()))) return nullptr;
    d.tempUavs.push_back(uav);
    return uav.get();
}

void bufferWritten(Buffer* b, VkDeviceSize off, VkDeviceSize len) {
    if (!b || !b->mem) return;
    b->mem->markDirty(b->memOffset + off, len);
    b->version++;
    b->lastSyncSubmit = 0;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateBufferView(VkDevice, const VkBufferViewCreateInfo* ci,
                                                const VkAllocationCallbacks*, VkBufferView* out) {
    BufferView* v = new BufferView();
    v->ci = *ci;
    v->buffer = from<Buffer>(ci->buffer);
    *out = to<VkBufferView>(v);
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL DestroyBufferView(VkDevice, VkBufferView h, const VkAllocationCallbacks*) {
    delete from<BufferView>(h);
}

// ---------------------------------------------------------------------------
// Images
Image* createImageObject(Device& d, const VkImageCreateInfo& ci, ID3D11Resource* existing) {
    const FormatInfo* fi = findFormat(ci.format);
    if (!fi || (fi->flags & FMT_VERTEX_ONLY)) return nullptr;
    auto img = std::make_unique<Image>();
    img->ci = ci;
    img->ci.pNext = nullptr;
    img->ci.pQueueFamilyIndices = nullptr;
    img->fmt = fi;
    if (existing) {
        img->tex = ComPtr<ID3D11Resource>(existing);
        return img.release();
    }
    UINT support = 0;
    d.d3d->CheckFormatSupport(fi->dxgi, &support);
    UINT bind = D3D11_BIND_SHADER_RESOURCE;
    const VkImageUsageFlags u = ci.usage;
    if (fi->flags & FMT_DEPTH) {
        if (u & (VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) bind |= D3D11_BIND_DEPTH_STENCIL;
    } else if (!(fi->flags & FMT_COMPRESSED)) {
        bool wantRT = u & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        bool canRT = (support & D3D11_FORMAT_SUPPORT_RENDER_TARGET) &&
                     (ci.imageType != VK_IMAGE_TYPE_1D || (u & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT));
        if (wantRT && canRT) bind |= D3D11_BIND_RENDER_TARGET;
        if (u & VK_IMAGE_USAGE_STORAGE_BIT) bind |= D3D11_BIND_UNORDERED_ACCESS;
    }
    HRESULT hr = E_FAIL;
    if (ci.imageType == VK_IMAGE_TYPE_1D) {
        D3D11_TEXTURE1D_DESC td{};
        td.Width = ci.extent.width;
        td.MipLevels = ci.mipLevels;
        td.ArraySize = ci.arrayLayers;
        td.Format = fi->typeless;
        td.BindFlags = bind;
        ComPtr<ID3D11Texture1D> t;
        hr = d.d3d->CreateTexture1D(&td, nullptr, t.put());
        img->tex = ComPtr<ID3D11Resource>(t.get());
    } else if (ci.imageType == VK_IMAGE_TYPE_2D) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = ci.extent.width;
        td.Height = ci.extent.height;
        td.MipLevels = ci.mipLevels;
        td.ArraySize = ci.arrayLayers;
        td.Format = fi->typeless;
        td.SampleDesc.Count = (UINT)ci.samples;
        td.BindFlags = bind;
        if ((ci.flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) && ci.arrayLayers >= 6) td.MiscFlags |= D3D11_RESOURCE_MISC_TEXTURECUBE;
        ComPtr<ID3D11Texture2D> t;
        hr = d.d3d->CreateTexture2D(&td, nullptr, t.put());
        img->tex = ComPtr<ID3D11Resource>(t.get());
    } else {
        D3D11_TEXTURE3D_DESC td{};
        td.Width = ci.extent.width;
        td.Height = ci.extent.height;
        td.Depth = ci.extent.depth;
        td.MipLevels = ci.mipLevels;
        td.Format = fi->typeless;
        td.BindFlags = bind;
        ComPtr<ID3D11Texture3D> t;
        hr = d.d3d->CreateTexture3D(&td, nullptr, t.put());
        img->tex = ComPtr<ID3D11Resource>(t.get());
    }
    if (FAILED(hr)) {
        VKON_LOG("CreateTexture failed: vkformat=%d %ux%ux%u mips=%u layers=%u bind=%x hr=0x%08lx", ci.format,
                 ci.extent.width, ci.extent.height, ci.extent.depth, ci.mipLevels, ci.arrayLayers, bind,
                 (unsigned long)hr);
        return nullptr;
    }
    return img.release();
}

VKAPI_ATTR VkResult VKAPI_CALL CreateImage(VkDevice h, const VkImageCreateInfo* ci, const VkAllocationCallbacks*,
                                           VkImage* out) {
    Image* img = createImageObject(*from<Device>(h), *ci, nullptr);
    if (!img) return findFormat(ci->format) ? VK_ERROR_OUT_OF_DEVICE_MEMORY : VK_ERROR_FORMAT_NOT_SUPPORTED;
    *out = to<VkImage>(img);
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL DestroyImage(VkDevice, VkImage h, const VkAllocationCallbacks*) { delete from<Image>(h); }

VKAPI_ATTR void VKAPI_CALL GetImageMemoryRequirements(VkDevice, VkImage h, VkMemoryRequirements* r) {
    Image* img = from<Image>(h);
    VkDeviceSize total = 0;
    for (uint32_t m = 0; m < img->ci.mipLevels; m++) {
        VkDeviceSize w = std::max(1u, img->ci.extent.width >> m), hgt = std::max(1u, img->ci.extent.height >> m),
                     dep = std::max(1u, img->ci.extent.depth >> m);
        VkDeviceSize b = img->fmt->block;
        total += ((w + b - 1) / b) * ((hgt + b - 1) / b) * dep * img->fmt->bytes;
    }
    total *= img->ci.arrayLayers * (VkDeviceSize)img->ci.samples;
    r->size = (total + PAGE - 1) & ~(PAGE - 1);
    r->alignment = PAGE;
    r->memoryTypeBits = 1; // device-local only: the texture lives in D3D11, the allocation is bookkeeping
}
VKAPI_ATTR VkResult VKAPI_CALL BindImageMemory(VkDevice, VkImage, VkDeviceMemory, VkDeviceSize) { return VK_SUCCESS; }
VKAPI_ATTR void VKAPI_CALL GetImageSparseMemoryRequirements(VkDevice, VkImage, uint32_t* count,
                                                            VkSparseImageMemoryRequirements*) { *count = 0; }
VKAPI_ATTR VkResult VKAPI_CALL QueueBindSparse(VkQueue, uint32_t, const VkBindSparseInfo*, VkFence) {
    return VK_ERROR_FEATURE_NOT_PRESENT;
}
VKAPI_ATTR void VKAPI_CALL GetImageSubresourceLayout(VkDevice, VkImage, const VkImageSubresource*,
                                                     VkSubresourceLayout* l) { memset(l, 0, sizeof(*l)); }

VKAPI_ATTR VkResult VKAPI_CALL CreateImageView(VkDevice, const VkImageViewCreateInfo* ci, const VkAllocationCallbacks*,
                                               VkImageView* out) {
    ImageView* v = new ImageView();
    v->ci = *ci;
    v->ci.pNext = nullptr;
    v->image = from<Image>(ci->image);
    v->fmt = findFormat(ci->format);
    if (!v->fmt) v->fmt = v->image->fmt;
    const auto& r = ci->subresourceRange;
    v->levelCount = r.levelCount == VK_REMAINING_MIP_LEVELS ? v->image->ci.mipLevels - r.baseMipLevel : r.levelCount;
    v->layerCount = r.layerCount == VK_REMAINING_ARRAY_LAYERS ? v->image->ci.arrayLayers - r.baseArrayLayer : r.layerCount;
    *out = to<VkImageView>(v);
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL DestroyImageView(VkDevice, VkImageView h, const VkAllocationCallbacks*) {
    delete from<ImageView>(h);
}

ID3D11ShaderResourceView* viewSRV(Device& d, ImageView* v) {
    if (!v) return nullptr;
    if (v->srv) return v->srv.get();
    const Image& img = *v->image;
    const auto& r = v->ci.subresourceRange;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = v->fmt->srv;
    if ((v->fmt->flags & FMT_STENCIL) && r.aspectMask == VK_IMAGE_ASPECT_STENCIL_BIT)
        sd.Format = v->fmt->vk == VK_FORMAT_D24_UNORM_S8_UINT ? DXGI_FORMAT_X24_TYPELESS_G8_UINT : DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
    const bool ms = img.ci.samples != VK_SAMPLE_COUNT_1_BIT;
    switch (v->ci.viewType) {
    case VK_IMAGE_VIEW_TYPE_1D:
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE1D;
        sd.Texture1D = {r.baseMipLevel, v->levelCount};
        if (r.baseArrayLayer) {
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE1DARRAY;
            sd.Texture1DArray = {r.baseMipLevel, v->levelCount, r.baseArrayLayer, 1};
        }
        break;
    case VK_IMAGE_VIEW_TYPE_1D_ARRAY:
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE1DARRAY;
        sd.Texture1DArray = {r.baseMipLevel, v->levelCount, r.baseArrayLayer, v->layerCount};
        break;
    case VK_IMAGE_VIEW_TYPE_2D:
        if (ms) {
            if (r.baseArrayLayer) {
                sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY;
                sd.Texture2DMSArray = {r.baseArrayLayer, 1};
            } else sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
        } else if (r.baseArrayLayer) {
            // D3D11 can only expose layer N>0 through an array view.
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            sd.Texture2DArray = {r.baseMipLevel, v->levelCount, r.baseArrayLayer, 1};
        } else {
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sd.Texture2D = {r.baseMipLevel, v->levelCount};
        }
        break;
    case VK_IMAGE_VIEW_TYPE_2D_ARRAY:
        if (ms) {
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY;
            sd.Texture2DMSArray = {r.baseArrayLayer, v->layerCount};
        } else {
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            sd.Texture2DArray = {r.baseMipLevel, v->levelCount, r.baseArrayLayer, v->layerCount};
        }
        break;
    case VK_IMAGE_VIEW_TYPE_CUBE:
        if (r.baseArrayLayer) {
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBEARRAY;
            sd.TextureCubeArray = {r.baseMipLevel, v->levelCount, r.baseArrayLayer, 1};
        } else {
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
            sd.TextureCube = {r.baseMipLevel, v->levelCount};
        }
        break;
    case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY:
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBEARRAY;
        sd.TextureCubeArray = {r.baseMipLevel, v->levelCount, r.baseArrayLayer, v->layerCount / 6};
        break;
    default:
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
        sd.Texture3D = {r.baseMipLevel, v->levelCount};
        break;
    }
    if (FAILED(d.d3d->CreateShaderResourceView(img.tex.get(), &sd, v->srv.put()))) VKON_LOG("CreateShaderResourceView failed");
    return v->srv.get();
}

ID3D11RenderTargetView* viewRTV(Device& d, ImageView* v) {
    if (!v) return nullptr;
    if (v->rtv) return v->rtv.get();
    const Image& img = *v->image;
    const auto& r = v->ci.subresourceRange;
    D3D11_RENDER_TARGET_VIEW_DESC rd{};
    rd.Format = v->fmt->dxgi;
    const bool ms = img.ci.samples != VK_SAMPLE_COUNT_1_BIT;
    if (img.ci.imageType == VK_IMAGE_TYPE_3D) {
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE3D;
        rd.Texture3D = {r.baseMipLevel, 0, (UINT)-1};
    } else if (img.ci.imageType == VK_IMAGE_TYPE_1D) {
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE1DARRAY;
        rd.Texture1DArray = {r.baseMipLevel, r.baseArrayLayer, v->layerCount};
    } else if (ms) {
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY;
        rd.Texture2DMSArray = {r.baseArrayLayer, v->layerCount};
    } else {
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
        rd.Texture2DArray = {r.baseMipLevel, r.baseArrayLayer, v->layerCount};
    }
    if (FAILED(d.d3d->CreateRenderTargetView(img.tex.get(), &rd, v->rtv.put()))) VKON_LOG("CreateRenderTargetView failed");
    return v->rtv.get();
}

ID3D11DepthStencilView* viewDSV(Device& d, ImageView* v) {
    if (!v) return nullptr;
    if (v->dsv) return v->dsv.get();
    const Image& img = *v->image;
    const auto& r = v->ci.subresourceRange;
    D3D11_DEPTH_STENCIL_VIEW_DESC dd{};
    dd.Format = v->fmt->dxgi;
    if (img.ci.samples != VK_SAMPLE_COUNT_1_BIT) {
        dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY;
        dd.Texture2DMSArray = {r.baseArrayLayer, v->layerCount};
    } else {
        dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
        dd.Texture2DArray = {r.baseMipLevel, r.baseArrayLayer, v->layerCount};
    }
    if (FAILED(d.d3d->CreateDepthStencilView(img.tex.get(), &dd, v->dsv.put()))) VKON_LOG("CreateDepthStencilView failed");
    return v->dsv.get();
}

ID3D11UnorderedAccessView* viewUAV(Device& d, ImageView* v) {
    if (!v) return nullptr;
    if (v->uav) return v->uav.get();
    const Image& img = *v->image;
    const auto& r = v->ci.subresourceRange;
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = v->fmt->dxgi;
    if (img.ci.imageType == VK_IMAGE_TYPE_3D) {
        ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D;
        ud.Texture3D = {r.baseMipLevel, 0, (UINT)-1};
    } else if (v->ci.viewType == VK_IMAGE_VIEW_TYPE_2D && r.baseArrayLayer == 0) {
        ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        ud.Texture2D = {r.baseMipLevel};
    } else {
        ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
        ud.Texture2DArray = {r.baseMipLevel, r.baseArrayLayer, v->layerCount};
    }
    if (FAILED(d.d3d->CreateUnorderedAccessView(img.tex.get(), &ud, v->uav.put()))) VKON_LOG("CreateUnorderedAccessView failed");
    return v->uav.get();
}

// ---------------------------------------------------------------------------
// Samplers
static D3D11_TEXTURE_ADDRESS_MODE addr(VkSamplerAddressMode m) {
    switch (m) {
    case VK_SAMPLER_ADDRESS_MODE_REPEAT: return D3D11_TEXTURE_ADDRESS_WRAP;
    case VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT: return D3D11_TEXTURE_ADDRESS_MIRROR;
    case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER: return D3D11_TEXTURE_ADDRESS_BORDER;
    case VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE: return D3D11_TEXTURE_ADDRESS_MIRROR_ONCE;
    default: return D3D11_TEXTURE_ADDRESS_CLAMP;
    }
}

VKAPI_ATTR VkResult VKAPI_CALL CreateSampler(VkDevice h, const VkSamplerCreateInfo* ci, const VkAllocationCallbacks*,
                                             VkSampler* out) {
    Device& d = *from<Device>(h);
    D3D11_SAMPLER_DESC sd{};
    UINT f = 0;
    if (ci->anisotropyEnable) f = 0x55;
    else {
        if (ci->mipmapMode == VK_SAMPLER_MIPMAP_MODE_LINEAR) f |= 0x1;
        if (ci->magFilter == VK_FILTER_LINEAR) f |= 0x4;
        if (ci->minFilter == VK_FILTER_LINEAR) f |= 0x10;
    }
    if (ci->compareEnable) f |= 0x80;
    sd.Filter = (D3D11_FILTER)f;
    sd.AddressU = addr(ci->addressModeU);
    sd.AddressV = addr(ci->addressModeV);
    sd.AddressW = addr(ci->addressModeW);
    sd.MipLODBias = ci->mipLodBias;
    sd.MaxAnisotropy = ci->anisotropyEnable ? (UINT)std::max(1.0f, std::min(16.0f, ci->maxAnisotropy)) : 1;
    sd.ComparisonFunc = ci->compareEnable ? (D3D11_COMPARISON_FUNC)(ci->compareOp + 1) : D3D11_COMPARISON_NEVER;
    float a = (ci->borderColor == VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK || ci->borderColor == VK_BORDER_COLOR_INT_TRANSPARENT_BLACK) ? 0.f : 1.f;
    float c = (ci->borderColor == VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE || ci->borderColor == VK_BORDER_COLOR_INT_OPAQUE_WHITE) ? 1.f : 0.f;
    sd.BorderColor[0] = sd.BorderColor[1] = sd.BorderColor[2] = c;
    sd.BorderColor[3] = a;
    sd.MinLOD = ci->minLod;
    sd.MaxLOD = ci->maxLod;
    Sampler* s = new Sampler();
    if (FAILED(d.d3d->CreateSamplerState(&sd, s->d3d.put()))) { delete s; return VK_ERROR_OUT_OF_DEVICE_MEMORY; }
    *out = to<VkSampler>(s);
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL DestroySampler(VkDevice, VkSampler h, const VkAllocationCallbacks*) { delete from<Sampler>(h); }

// ---------------------------------------------------------------------------
// Sync objects. Command buffers execute synchronously inside vkQueueSubmit, so
// fences are signalled as soon as submission returns.
VKAPI_ATTR VkResult VKAPI_CALL CreateFence(VkDevice, const VkFenceCreateInfo* ci, const VkAllocationCallbacks*, VkFence* out) {
    Fence* f = new Fence();
    f->signaled = ci->flags & VK_FENCE_CREATE_SIGNALED_BIT;
    *out = to<VkFence>(f);
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL DestroyFence(VkDevice, VkFence h, const VkAllocationCallbacks*) { delete from<Fence>(h); }
VKAPI_ATTR VkResult VKAPI_CALL ResetFences(VkDevice, uint32_t n, const VkFence* f) {
    for (uint32_t i = 0; i < n; i++) from<Fence>(f[i])->signaled = false;
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL GetFenceStatus(VkDevice, VkFence f) {
    return from<Fence>(f)->signaled ? VK_SUCCESS : VK_NOT_READY;
}
VKAPI_ATTR VkResult VKAPI_CALL WaitForFences(VkDevice, uint32_t n, const VkFence* f, VkBool32 all, uint64_t timeout) {
    uint32_t done = 0;
    for (uint32_t i = 0; i < n; i++) done += from<Fence>(f[i])->signaled ? 1 : 0;
    if (all ? done == n : done > 0) return VK_SUCCESS;
    if (timeout) Sleep((DWORD)std::min<uint64_t>(timeout / 1000000, 50));
    return VK_TIMEOUT;
}
VKAPI_ATTR VkResult VKAPI_CALL CreateSemaphore(VkDevice, const VkSemaphoreCreateInfo*, const VkAllocationCallbacks*, VkSemaphore* out) {
    *out = to<VkSemaphore>(new Semaphore());
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL DestroySemaphore(VkDevice, VkSemaphore h, const VkAllocationCallbacks*) { delete from<Semaphore>(h); }
VKAPI_ATTR VkResult VKAPI_CALL CreateEvent(VkDevice, const VkEventCreateInfo*, const VkAllocationCallbacks*, VkEvent* out) {
    *out = to<VkEvent>(new Event());
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL DestroyEvent(VkDevice, VkEvent h, const VkAllocationCallbacks*) { delete from<Event>(h); }
VKAPI_ATTR VkResult VKAPI_CALL GetEventStatus(VkDevice, VkEvent h) { return from<Event>(h)->set ? VK_EVENT_SET : VK_EVENT_RESET; }
VKAPI_ATTR VkResult VKAPI_CALL SetEvent(VkDevice, VkEvent h) { from<Event>(h)->set = true; return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL ResetEvent(VkDevice, VkEvent h) { from<Event>(h)->set = false; return VK_SUCCESS; }

// ---------------------------------------------------------------------------
// Queries (occlusion only)
VKAPI_ATTR VkResult VKAPI_CALL CreateQueryPool(VkDevice h, const VkQueryPoolCreateInfo* ci, const VkAllocationCallbacks*, VkQueryPool* out) {
    Device& d = *from<Device>(h);
    QueryPool* p = new QueryPool();
    p->type = ci->queryType;
    p->queries.resize(ci->queryCount);
    p->active.assign(ci->queryCount, 0);
    if (ci->queryType == VK_QUERY_TYPE_OCCLUSION) {
        D3D11_QUERY_DESC qd{D3D11_QUERY_OCCLUSION, 0};
        for (auto& q : p->queries) d.d3d->CreateQuery(&qd, q.put());
    }
    *out = to<VkQueryPool>(p);
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL DestroyQueryPool(VkDevice, VkQueryPool h, const VkAllocationCallbacks*) { delete from<QueryPool>(h); }

VKAPI_ATTR VkResult VKAPI_CALL GetQueryPoolResults(VkDevice h, VkQueryPool ph, uint32_t first, uint32_t count,
                                                   size_t, void* data, VkDeviceSize stride, VkQueryResultFlags flags) {
    Device& d = *from<Device>(h);
    QueryPool* p = from<QueryPool>(ph);
    std::lock_guard<std::mutex> lk(d.phys->ctxMutex);
    VkResult res = VK_SUCCESS;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t value = 0;
        bool avail = false;
        ID3D11Query* q = first + i < p->queries.size() ? p->queries[first + i].get() : nullptr;
        if (q && p->active[first + i]) {
            for (int tries = 0; tries < 2000; tries++) {
                HRESULT hr = d.ctx->GetData(q, &value, sizeof(value), 0);
                if (hr == S_OK) { avail = true; break; }
                if (!(flags & VK_QUERY_RESULT_WAIT_BIT) || FAILED(hr)) break;
                Sleep(1);
            }
        }
        uint8_t* dst = (uint8_t*)data + i * stride;
        if (!avail && !(flags & VK_QUERY_RESULT_PARTIAL_BIT)) res = VK_NOT_READY;
        if (flags & VK_QUERY_RESULT_64_BIT) {
            if (avail || (flags & VK_QUERY_RESULT_PARTIAL_BIT)) memcpy(dst, &value, 8);
            if (flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) { uint64_t a = avail; memcpy(dst + 8, &a, 8); }
        } else {
            uint32_t v32 = (uint32_t)std::min<uint64_t>(value, 0xffffffffu);
            if (avail || (flags & VK_QUERY_RESULT_PARTIAL_BIT)) memcpy(dst, &v32, 4);
            if (flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) { uint32_t a = avail; memcpy(dst + 4, &a, 4); }
        }
    }
    return res;
}

} // namespace vkon11
