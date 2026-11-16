// Command buffers: Vulkan commands are recorded as closures and replayed on the
// D3D11 immediate context when the command buffer is submitted.
#include "vkon11.h"

namespace vkon11 {

static inline CommandBuffer* CB(VkCommandBuffer h) { return from<CommandBuffer>(h); }
template <class F> static inline void rec(VkCommandBuffer h, F&& f) { CB(h)->cmds.emplace_back(std::forward<F>(f)); }

// ---------------------------------------------------------------------------
// Pools / lifetime
VKAPI_ATTR VkResult VKAPI_CALL CreateCommandPool(VkDevice, const VkCommandPoolCreateInfo*, const VkAllocationCallbacks*,
                                                 VkCommandPool* out) {
    *out = to<VkCommandPool>(new CommandPool());
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL DestroyCommandPool(VkDevice, VkCommandPool h, const VkAllocationCallbacks*) {
    CommandPool* p = from<CommandPool>(h);
    if (!p) return;
    for (CommandBuffer* c : p->buffers) delete c;
    delete p;
}
VKAPI_ATTR VkResult VKAPI_CALL ResetCommandPool(VkDevice, VkCommandPool h, VkCommandPoolResetFlags) {
    for (CommandBuffer* c : from<CommandPool>(h)->buffers) c->cmds.clear();
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL TrimCommandPoolKHR(VkDevice, VkCommandPool, VkCommandPoolTrimFlags) {}
VKAPI_ATTR VkResult VKAPI_CALL AllocateCommandBuffers(VkDevice h, const VkCommandBufferAllocateInfo* ai,
                                                      VkCommandBuffer* out) {
    CommandPool* pool = from<CommandPool>(ai->commandPool);
    for (uint32_t i = 0; i < ai->commandBufferCount; i++) {
        CommandBuffer* c = new CommandBuffer();
        set_loader_magic_value(c);
        c->device = from<Device>(h);
        c->pool = pool;
        pool->buffers.push_back(c);
        out[i] = to<VkCommandBuffer>(c);
    }
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL FreeCommandBuffers(VkDevice, VkCommandPool h, uint32_t n, const VkCommandBuffer* bufs) {
    CommandPool* pool = from<CommandPool>(h);
    for (uint32_t i = 0; i < n; i++) {
        CommandBuffer* c = CB(bufs[i]);
        if (!c) continue;
        pool->buffers.erase(std::remove(pool->buffers.begin(), pool->buffers.end(), c), pool->buffers.end());
        delete c;
    }
}
VKAPI_ATTR VkResult VKAPI_CALL BeginCommandBuffer(VkCommandBuffer h, const VkCommandBufferBeginInfo*) {
    CB(h)->cmds.clear();
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL EndCommandBuffer(VkCommandBuffer) { return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL ResetCommandBuffer(VkCommandBuffer h, VkCommandBufferResetFlags) {
    CB(h)->cmds.clear();
    return VK_SUCCESS;
}

void executeCommandBuffer(Device& d, CommandBuffer* cb) {
    d.st = ExecState(); // Vulkan state does not carry over between command buffers
    for (Cmd& c : cb->cmds) c(d);
    d.ctx->OMSetRenderTargets(0, nullptr, nullptr);
}

// ---------------------------------------------------------------------------
// State flush
static ID3D11Buffer* dynamicCB(Device& d, ComPtr<ID3D11Buffer>& cb, UINT size) {
    if (!cb) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = size;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        d.d3d->CreateBuffer(&bd, nullptr, cb.put());
    }
    return cb.get();
}
static void writeCB(Device& d, ID3D11Buffer* cb, const void* data, size_t size) {
    D3D11_MAPPED_SUBRESOURCE m;
    if (cb && SUCCEEDED(d.ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, data, size);
        d.ctx->Unmap(cb, 0);
    }
}

static void bindDescriptors(Device& d, int bp) {
    ExecState& st = d.st;
    Pipeline* p = st.pipeline[bp];
    const PipelineLayoutData& L = *p->layout;
    ID3D11DeviceContext* c = d.ctx;
    const int first = bp == 0 ? ST_VS : ST_CS, last = bp == 0 ? ST_PS : ST_CS;
    for (int s = first; s <= last; s++) {
        ID3D11Buffer* cbv[MAX_USER_CBV] = {};
        ID3D11ShaderResourceView* srv[MAX_SRV] = {};
        ID3D11SamplerState* smp[MAX_SAMPLER] = {};
        ID3D11UnorderedAccessView* uav[MAX_UAV] = {};
        for (const SlotMap& sm : L.slots[s]) {
            DescriptorSet* set = st.sets[bp][sm.set];
            if (!set) continue;
            const SetBinding& b = L.sets[sm.set]->bindings[sm.bindingIndex];
            for (uint32_t i = 0; i < b.count; i++) {
                if (b.offset + i >= set->desc.size()) break;
                const Descriptor& de = set->desc[b.offset + i];
                switch (b.type) {
                case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
                case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC: {
                    VkDeviceSize off = de.offset;
                    if (b.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC && b.dynIndex + i < st.dynOffsets[bp][sm.set].size())
                        off += st.dynOffsets[bp][sm.set][b.dynIndex + i];
                    if (sm.cbv + i < MAX_USER_CBV) cbv[sm.cbv + i] = d.uniformCB(de.buffer, off, de.range);
                    break;
                }
                case VK_DESCRIPTOR_TYPE_SAMPLER:
                    if (sm.sampler + i < MAX_SAMPLER && de.sampler) smp[sm.sampler + i] = de.sampler->d3d.get();
                    break;
                case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
                    if (sm.sampler + i < MAX_SAMPLER && de.sampler) smp[sm.sampler + i] = de.sampler->d3d.get();
                    if (sm.srv + i < MAX_SRV) srv[sm.srv + i] = viewSRV(d, de.view);
                    break;
                case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
                case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
                    if (sm.srv + i < MAX_SRV) srv[sm.srv + i] = viewSRV(d, de.view);
                    break;
                case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
                    if (sm.uav + i < MAX_UAV) uav[sm.uav + i] = viewUAV(d, de.view);
                    break;
                case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
                case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: {
                    VkDeviceSize off = de.offset;
                    if (b.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC && b.dynIndex + i < st.dynOffsets[bp][sm.set].size())
                        off += st.dynOffsets[bp][sm.set][b.dynIndex + i];
                    if (sm.uav + i < MAX_UAV && s == ST_CS) {
                        uav[sm.uav + i] = bufferUAV(d, de.buffer, off, de.range);
                        if (uav[sm.uav + i]) d.pendingUavBuffers.push_back(de.buffer);
                    }
                    break;
                }
                default: break;
                }
            }
        }
        const UINT nc = std::min<UINT>(L.cbvCount[s], MAX_USER_CBV), nt = std::min<UINT>(L.srvCount[s], MAX_SRV),
                   ns = std::min<UINT>(L.samplerCount[s], MAX_SAMPLER), nu = std::min<UINT>(L.uavCount[s], MAX_UAV);
        if (s == ST_VS) {
            if (nc) c->VSSetConstantBuffers(0, nc, cbv);
            if (nt) c->VSSetShaderResources(0, nt, srv);
            if (ns) c->VSSetSamplers(0, ns, smp);
        } else if (s == ST_PS) {
            if (nc) c->PSSetConstantBuffers(0, nc, cbv);
            if (nt) c->PSSetShaderResources(0, nt, srv);
            if (ns) c->PSSetSamplers(0, ns, smp);
        } else {
            if (nc) c->CSSetConstantBuffers(0, nc, cbv);
            if (nt) c->CSSetShaderResources(0, nt, srv);
            if (ns) c->CSSetSamplers(0, ns, smp);
            if (nu) c->CSSetUnorderedAccessViews(0, nu, uav, nullptr);
        }
    }
    st.dirtyDesc[bp] = false;
}

void flushGraphics(Device& d) {
    ExecState& st = d.st;
    Pipeline* p = st.pipeline[0];
    ID3D11DeviceContext* c = d.ctx;
    auto dyn = [&](VkDynamicState s) { return (p->dynamicMask & (1u << s)) != 0; };

    if (st.dirtyPipeline) {
        c->VSSetShader(p->vs.get(), nullptr, 0);
        c->PSSetShader(p->ps.get(), nullptr, 0);
        c->GSSetShader(nullptr, nullptr, 0);
        c->HSSetShader(nullptr, nullptr, 0);
        c->DSSetShader(nullptr, nullptr, 0);
        c->IASetInputLayout(p->inputLayout.get());
        c->IASetPrimitiveTopology(p->topology);
        ID3D11Buffer* pc = dynamicCB(d, d.pushCB, 256);
        c->VSSetConstantBuffers(SLOT_PUSH_CONSTANTS, 1, &pc);
        c->PSSetConstantBuffers(SLOT_PUSH_CONSTANTS, 1, &pc);
        ID3D11Buffer* vi = dynamicCB(d, d.vertexInfoCB, 16);
        c->VSSetConstantBuffers(SLOT_VERTEX_INFO, 1, &vi);
        st.dirtyDynamic = st.dirtyVB = st.dirtyDesc[0] = true;
        st.dirtyPipeline = false;
    }
    if (st.dirtyDynamic) {
        c->OMSetBlendState(p->blend.get(), dyn(VK_DYNAMIC_STATE_BLEND_CONSTANTS) ? st.blendConstants : p->blendConstants, p->sampleMask);

        const bool dsDyn = dyn(VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK) || dyn(VK_DYNAMIC_STATE_STENCIL_WRITE_MASK);
        ComPtr<ID3D11DepthStencilState> dss = p->dsState;
        if (dsDyn || !dss) {
            D3D11_DEPTH_STENCIL_DESC ds = p->dsDesc;
            if (dyn(VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK)) ds.StencilReadMask = (UINT8)st.stencilCompareMask;
            if (dyn(VK_DYNAMIC_STATE_STENCIL_WRITE_MASK)) ds.StencilWriteMask = (UINT8)st.stencilWriteMask;
            d.d3d->CreateDepthStencilState(&ds, dss.put());
            if (!dsDyn) p->dsState = dss;
        }
        c->OMSetDepthStencilState(dss.get(), dyn(VK_DYNAMIC_STATE_STENCIL_REFERENCE) ? st.stencilRef : p->stencilRef);

        const bool rsDyn = dyn(VK_DYNAMIC_STATE_DEPTH_BIAS);
        ComPtr<ID3D11RasterizerState> rss = p->rsState;
        if (rsDyn || !rss) {
            D3D11_RASTERIZER_DESC rs = p->rsDesc;
            if (rsDyn) {
                rs.DepthBias = (INT)st.depthBias[0];
                rs.DepthBiasClamp = st.depthBias[1];
                rs.SlopeScaledDepthBias = st.depthBias[2];
            }
            d.d3d->CreateRasterizerState(&rs, rss.put());
            if (!rsDyn) p->rsState = rss;
        }
        c->RSSetState(rss.get());

        const auto& vps = dyn(VK_DYNAMIC_STATE_VIEWPORT) ? st.viewports : p->viewports;
        const auto& scs = dyn(VK_DYNAMIC_STATE_SCISSOR) ? st.scissors : p->scissors;
        D3D11_VIEWPORT dv[16];
        D3D11_RECT dr[16];
        UINT nv = (UINT)std::min<size_t>(vps.size(), 16), nr = (UINT)std::min<size_t>(scs.size(), 16);
        st.yflip = (nv && vps[0].height < 0) ? 1.0f : -1.0f;
        for (UINT i = 0; i < nv; i++) {
            float y = vps[i].y, h = vps[i].height;
            if (h < 0) { y += h; h = -h; }
            dv[i] = {vps[i].x, y, vps[i].width, h, std::max(0.f, std::min(1.f, vps[i].minDepth)),
                     std::max(0.f, std::min(1.f, vps[i].maxDepth))};
            if (dv[i].MinDepth > dv[i].MaxDepth) std::swap(dv[i].MinDepth, dv[i].MaxDepth);
        }
        for (UINT i = 0; i < nr; i++)
            dr[i] = {scs[i].offset.x, scs[i].offset.y, (LONG)(scs[i].offset.x + scs[i].extent.width),
                     (LONG)(scs[i].offset.y + scs[i].extent.height)};
        if (nv) c->RSSetViewports(nv, dv);
        if (nr) c->RSSetScissorRects(nr, dr);
        st.dirtyDynamic = false;
    }
    if (st.dirtyVB) {
        for (uint32_t b = 0; b < 32; b++) {
            if (!(p->vbMask & (1u << b))) continue;
            ID3D11Buffer* buf = bufferGpu(d, st.vb[b]);
            UINT stride = p->strides[b], off = (UINT)st.vbOffset[b];
            c->IASetVertexBuffers(b, 1, &buf, &stride, &off);
        }
        st.dirtyVB = false;
    }
    if (st.dirtyDesc[0]) bindDescriptors(d, 0);
    if (st.dirtyPush) {
        writeCB(d, dynamicCB(d, d.pushCB, 256), st.push, 256);
        st.dirtyPush = false;
    }
}

static bool prepareDraw(Device& d, bool indexed, int baseVertex, int baseInstance) {
    ExecState& st = d.st;
    Pipeline* p = st.pipeline[0];
    if (!p || p->compute || p->rasterDiscard || !st.rp) return false;
    flushGraphics(d);
    if (indexed && st.dirtyIB) {
        d.ctx->IASetIndexBuffer(bufferGpu(d, st.ib), st.ibType == VK_INDEX_TYPE_UINT32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT,
                                (UINT)st.ibOffset);
        st.dirtyIB = false;
    }
    if (st.yflip != st.lastYflip || (p->usesVertexInfo && (baseVertex != st.lastBaseVertex || baseInstance != st.lastBaseInstance))) {
        struct { int baseVertex, baseInstance; float yflip; int pad; } info = {baseVertex, baseInstance, st.yflip, 0};
        writeCB(d, dynamicCB(d, d.vertexInfoCB, 16), &info, sizeof(info));
        st.lastBaseVertex = baseVertex;
        st.lastBaseInstance = baseInstance;
        st.lastYflip = st.yflip;
    }
    return true;
}

static void draw(Device& d, uint32_t vc, uint32_t ic, uint32_t fv, uint32_t fi) {
    if (!vc || !ic || !prepareDraw(d, false, (int)fv, (int)fi)) return;
    if (ic == 1 && fi == 0) d.ctx->Draw(vc, fv);
    else d.ctx->DrawInstanced(vc, ic, fv, fi);
}
static void drawIndexed(Device& d, uint32_t n, uint32_t ic, uint32_t first, int32_t vo, uint32_t fi) {
    if (!n || !ic || !prepareDraw(d, true, vo, (int)fi)) return;
    if (ic == 1 && fi == 0) d.ctx->DrawIndexed(n, first, vo);
    else d.ctx->DrawIndexedInstanced(n, ic, first, vo, fi);
}

// The driver changed a buffer's contents during replay.
static void wrote(Device& d, Buffer* b, VkDeviceSize off, VkDeviceSize len) {
    bufferWritten(b, off, len);
    d.st.dirtyVB = d.st.dirtyIB = d.st.dirtyDesc[0] = d.st.dirtyDesc[1] = true;
}

// ---------------------------------------------------------------------------
// Rectangle clears. D3D11 clears whole views only, so partial clears are drawn
// as a scissored full-screen triangle.
static const char* kClearHlsl =
    "cbuffer C : register(b0) { float4 color; int4 icolor; float depth; };\n"
    "float4 vs(uint id : SV_VertexID) : SV_Position { float2 t = float2((id << 1) & 2, id & 2);\n"
    "  return float4(t * float2(2, -2) + float2(-1, 1), depth, 1); }\n"
    "float4 psf() : SV_Target { return color; }\n"
    "int4 psi() : SV_Target { return icolor; }\n"
    "uint4 psu() : SV_Target { return (uint4)icolor; }\n";

static bool initClear(Device& d) {
    if (d.clearVS && d.clearPS[0]) return true;
    std::vector<uint8_t> code;
    std::string err;
    if (!compileHLSL(kClearHlsl, "vs", "vs_5_0", code, err)) { VKON_LOG("clear shader compile failed: %s", err.c_str()); return false; }
    d.d3d->CreateVertexShader(code.data(), code.size(), nullptr, d.clearVS.put());
    static const char* entries[3] = {"psf", "psi", "psu"};
    for (int i = 0; i < 3; i++) {
        if (!compileHLSL(kClearHlsl, entries[i], "ps_5_0", code, err)) { VKON_LOG("clear shader compile failed: %s", err.c_str()); return false; }
        d.d3d->CreatePixelShader(code.data(), code.size(), nullptr, d.clearPS[i].put());
    }
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.ScissorEnable = TRUE;
    d.d3d->CreateRasterizerState(&rd, d.clearRS.put());
    return d.clearVS && d.clearPS[0] && d.clearPS[1] && d.clearPS[2];
}

static bool isSint(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8_SINT: case DXGI_FORMAT_R8G8_SINT: case DXGI_FORMAT_R8G8B8A8_SINT: case DXGI_FORMAT_R16_SINT:
    case DXGI_FORMAT_R16G16_SINT: case DXGI_FORMAT_R16G16B16A16_SINT: case DXGI_FORMAT_R32_SINT:
    case DXGI_FORMAT_R32G32_SINT: case DXGI_FORMAT_R32G32B32A32_SINT: return true;
    default: return false;
    }
}

// Clears `rects` of one colour view or of a depth/stencil view. Leaves the
// output merger pointing at the cleared view; callers rebind their targets.
static void clearRects(Device& d, ImageView* v, const VkClearValue& cv, UINT dsFlags, const D3D11_RECT* rects, UINT count) {
    if (!count || !initClear(d)) return;
    ID3D11DeviceContext* c = d.ctx;
    const bool depth = (v->fmt->flags & FMT_DEPTH) != 0;
    struct { float color[4]; int32_t icolor[4]; float depth; float pad[3]; } cb{};
    memcpy(cb.color, cv.color.float32, 16);
    memcpy(cb.icolor, cv.color.int32, 16);
    cb.depth = depth ? cv.depthStencil.depth : 0.0f;
    ID3D11Buffer* buf = dynamicCB(d, d.clearCB, sizeof(cb));
    writeCB(d, buf, &cb, sizeof(cb));

    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    dd.FrontFace = dd.BackFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS};
    if (depth) {
        dd.DepthEnable = TRUE;
        dd.DepthWriteMask = (dsFlags & D3D11_CLEAR_DEPTH) ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
        if (dsFlags & D3D11_CLEAR_STENCIL) {
            dd.StencilEnable = TRUE;
            dd.StencilReadMask = dd.StencilWriteMask = 0xff;
            dd.FrontFace = dd.BackFace = {D3D11_STENCIL_OP_REPLACE, D3D11_STENCIL_OP_REPLACE, D3D11_STENCIL_OP_REPLACE, D3D11_COMPARISON_ALWAYS};
        }
    }
    ComPtr<ID3D11DepthStencilState> dss;
    d.d3d->CreateDepthStencilState(&dd, dss.put());
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ComPtr<ID3D11BlendState> blend;
    d.d3d->CreateBlendState(&bd, blend.put());

    const uint32_t mip = v->ci.subresourceRange.baseMipLevel;
    D3D11_VIEWPORT vp{0, 0, (float)std::max(1u, v->image->ci.extent.width >> mip), (float)std::max(1u, v->image->ci.extent.height >> mip), 0, 1};
    if (depth) {
        c->OMSetRenderTargets(0, nullptr, viewDSV(d, v));
        c->PSSetShader(nullptr, nullptr, 0);
    } else {
        ID3D11RenderTargetView* rtv = viewRTV(d, v);
        c->OMSetRenderTargets(1, &rtv, nullptr);
        int kind = !(v->fmt->flags & FMT_INT) ? 0 : isSint(v->fmt->dxgi) ? 1 : 2;
        c->PSSetShader(d.clearPS[kind].get(), nullptr, 0);
        c->PSSetConstantBuffers(0, 1, &buf);
    }
    c->OMSetBlendState(blend.get(), nullptr, 0xffffffff);
    c->OMSetDepthStencilState(dss.get(), depth ? cv.depthStencil.stencil : 0);
    c->RSSetState(d.clearRS.get());
    c->RSSetViewports(1, &vp);
    c->IASetInputLayout(nullptr);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->VSSetShader(d.clearVS.get(), nullptr, 0);
    c->GSSetShader(nullptr, nullptr, 0);
    c->HSSetShader(nullptr, nullptr, 0);
    c->DSSetShader(nullptr, nullptr, 0);
    c->VSSetConstantBuffers(0, 1, &buf);
    for (UINT i = 0; i < count; i++) {
        c->RSSetScissorRects(1, &rects[i]);
        c->Draw(3, 0);
    }
    d.st.invalidate();
}

// ---------------------------------------------------------------------------
// Render passes
static bool coversFramebuffer(const ExecState& st, const VkRect2D& r) {
    return r.offset.x <= 0 && r.offset.y <= 0 && r.offset.x + (int64_t)r.extent.width >= st.fb->width &&
           r.offset.y + (int64_t)r.extent.height >= st.fb->height;
}
static D3D11_RECT toRect(const VkRect2D& r) {
    return {r.offset.x, r.offset.y, (LONG)(r.offset.x + r.extent.width), (LONG)(r.offset.y + r.extent.height)};
}

static void clearAttachment(Device& d, uint32_t a) {
    ExecState& st = d.st;
    if (a >= st.cleared.size() || st.cleared[a]) return;
    st.cleared[a] = 1;
    const VkAttachmentDescription& desc = st.rp->attachments[a];
    ImageView* v = st.fb->views[a];
    VkClearValue cv{};
    if (a < st.clears.size()) cv = st.clears[a];
    const bool full = coversFramebuffer(st, st.area);
    const D3D11_RECT area = toRect(st.area);
    if (v->fmt->flags & FMT_DEPTH) {
        UINT flags = 0;
        if (desc.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR) flags |= D3D11_CLEAR_DEPTH;
        if ((v->fmt->flags & FMT_STENCIL) && desc.stencilLoadOp == VK_ATTACHMENT_LOAD_OP_CLEAR) flags |= D3D11_CLEAR_STENCIL;
        if (!flags) return;
        if (!full) clearRects(d, v, cv, flags, &area, 1);
        else if (ID3D11DepthStencilView* dsv = viewDSV(d, v))
            d.ctx->ClearDepthStencilView(dsv, flags, cv.depthStencil.depth, (UINT8)cv.depthStencil.stencil);
        return;
    }
    if (desc.loadOp != VK_ATTACHMENT_LOAD_OP_CLEAR) return;
    if (!full) { clearRects(d, v, cv, 0, &area, 1); return; }
    ID3D11RenderTargetView* rtv = viewRTV(d, v);
    if (!rtv) return;
    float col[4];
    for (int i = 0; i < 4; i++) {
        // ClearRenderTargetView always takes floats; integer targets get the value cast back.
        if (v->fmt->flags & FMT_INT) col[i] = (float)cv.color.int32[i];
        else col[i] = cv.color.float32[i];
    }
    d.ctx->ClearRenderTargetView(rtv, col);
}

static void setSubpassTargets(Device& d) {
    ExecState& st = d.st;
    const SubpassData& sp = st.rp->subpasses[st.subpass];
    ID3D11RenderTargetView* rtv[8] = {};
    UINT n = (UINT)std::min<size_t>(sp.color.size(), 8);
    for (UINT i = 0; i < n; i++) {
        uint32_t a = sp.color[i].attachment;
        if (a != VK_ATTACHMENT_UNUSED && a < st.fb->views.size()) rtv[i] = viewRTV(d, st.fb->views[a]);
    }
    ID3D11DepthStencilView* dsv = nullptr;
    if (sp.depth.attachment != VK_ATTACHMENT_UNUSED && sp.depth.attachment < st.fb->views.size())
        dsv = viewDSV(d, st.fb->views[sp.depth.attachment]);
    d.ctx->OMSetRenderTargets(n, rtv, dsv);
    st.dirtyDesc[0] = true; // D3D11 unbinds SRVs that alias the new render targets
}

static void bindSubpass(Device& d) {
    ExecState& st = d.st;
    const SubpassData& sp = st.rp->subpasses[st.subpass];
    for (const VkAttachmentReference& r : sp.color)
        if (r.attachment != VK_ATTACHMENT_UNUSED && r.attachment < st.fb->views.size()) clearAttachment(d, r.attachment);
    if (sp.depth.attachment != VK_ATTACHMENT_UNUSED && sp.depth.attachment < st.fb->views.size()) clearAttachment(d, sp.depth.attachment);
    setSubpassTargets(d);
}

static void resolveSubpass(Device& d) {
    ExecState& st = d.st;
    const SubpassData& sp = st.rp->subpasses[st.subpass];
    for (size_t i = 0; i < sp.resolve.size() && i < sp.color.size(); i++) {
        uint32_t src = sp.color[i].attachment, dst = sp.resolve[i].attachment;
        if (src == VK_ATTACHMENT_UNUSED || dst == VK_ATTACHMENT_UNUSED) continue;
        ImageView *sv = st.fb->views[src], *dv = st.fb->views[dst];
        for (uint32_t l = 0; l < std::min(sv->layerCount, dv->layerCount); l++)
            d.ctx->ResolveSubresource(dv->image->tex.get(),
                                      dv->image->sub(dv->ci.subresourceRange.baseMipLevel, dv->ci.subresourceRange.baseArrayLayer + l),
                                      sv->image->tex.get(),
                                      sv->image->sub(sv->ci.subresourceRange.baseMipLevel, sv->ci.subresourceRange.baseArrayLayer + l),
                                      dv->fmt->dxgi);
    }
}

VKAPI_ATTR void VKAPI_CALL CmdBeginRenderPass(VkCommandBuffer h, const VkRenderPassBeginInfo* bi, VkSubpassContents) {
    RenderPass* rp = from<RenderPass>(bi->renderPass);
    Framebuffer* fb = from<Framebuffer>(bi->framebuffer);
    VkRect2D area = bi->renderArea;
    std::vector<VkClearValue> clears(bi->pClearValues, bi->pClearValues + (bi->pClearValues ? bi->clearValueCount : 0));
    rec(h, [=](Device& d) {
        ExecState& st = d.st;
        st.rp = rp;
        st.fb = fb;
        st.subpass = 0;
        st.area = area;
        st.clears = clears;
        st.cleared.assign(rp->attachments.size(), 0);
        bindSubpass(d);
    });
}
VKAPI_ATTR void VKAPI_CALL CmdNextSubpass(VkCommandBuffer h, VkSubpassContents) {
    rec(h, [](Device& d) {
        if (!d.st.rp) return;
        resolveSubpass(d);
        d.st.subpass++;
        bindSubpass(d);
    });
}
VKAPI_ATTR void VKAPI_CALL CmdEndRenderPass(VkCommandBuffer h) {
    rec(h, [](Device& d) {
        if (!d.st.rp) return;
        // Attachments that are cleared but never drawn to still need their clear.
        for (uint32_t a = 0; a < d.st.cleared.size(); a++) clearAttachment(d, a);
        resolveSubpass(d);
        d.ctx->OMSetRenderTargets(0, nullptr, nullptr);
        d.st.rp = nullptr;
        d.st.fb = nullptr;
    });
}
VKAPI_ATTR void VKAPI_CALL CmdExecuteCommands(VkCommandBuffer h, uint32_t n, const VkCommandBuffer* bufs) {
    std::vector<CommandBuffer*> list;
    for (uint32_t i = 0; i < n; i++) list.push_back(CB(bufs[i]));
    rec(h, [=](Device& d) {
        for (CommandBuffer* s : list)
            for (Cmd& c : s->cmds) c(d);
    });
}

// ---------------------------------------------------------------------------
// Binding / dynamic state
VKAPI_ATTR void VKAPI_CALL CmdBindPipeline(VkCommandBuffer h, VkPipelineBindPoint bp, VkPipeline ph) {
    Pipeline* p = from<Pipeline>(ph);
    rec(h, [=](Device& d) {
        int i = bp == VK_PIPELINE_BIND_POINT_COMPUTE ? 1 : 0;
        if (d.st.pipeline[i] == p) return;
        d.st.pipeline[i] = p;
        d.st.dirtyDesc[i] = true;
        if (i == 0) d.st.dirtyPipeline = true;
    });
}
VKAPI_ATTR void VKAPI_CALL CmdSetViewport(VkCommandBuffer h, uint32_t first, uint32_t n, const VkViewport* v) {
    std::vector<VkViewport> vps(v, v + n);
    rec(h, [=](Device& d) {
        if (d.st.viewports.size() < first + vps.size()) d.st.viewports.resize(first + vps.size());
        std::copy(vps.begin(), vps.end(), d.st.viewports.begin() + first);
        d.st.dirtyDynamic = true;
    });
}
VKAPI_ATTR void VKAPI_CALL CmdSetScissor(VkCommandBuffer h, uint32_t first, uint32_t n, const VkRect2D* s) {
    std::vector<VkRect2D> scs(s, s + n);
    rec(h, [=](Device& d) {
        if (d.st.scissors.size() < first + scs.size()) d.st.scissors.resize(first + scs.size());
        std::copy(scs.begin(), scs.end(), d.st.scissors.begin() + first);
        d.st.dirtyDynamic = true;
    });
}
VKAPI_ATTR void VKAPI_CALL CmdSetLineWidth(VkCommandBuffer, float) {}
VKAPI_ATTR void VKAPI_CALL CmdSetDepthBounds(VkCommandBuffer, float, float) {}
VKAPI_ATTR void VKAPI_CALL CmdSetDepthBias(VkCommandBuffer h, float constant, float clamp, float slope) {
    rec(h, [=](Device& d) {
        d.st.depthBias[0] = constant; d.st.depthBias[1] = clamp; d.st.depthBias[2] = slope;
        d.st.dirtyDynamic = true;
    });
}
VKAPI_ATTR void VKAPI_CALL CmdSetBlendConstants(VkCommandBuffer h, const float c[4]) {
    float v[4] = {c[0], c[1], c[2], c[3]};
    rec(h, [=](Device& d) { memcpy(d.st.blendConstants, v, sizeof(v)); d.st.dirtyDynamic = true; });
}
VKAPI_ATTR void VKAPI_CALL CmdSetStencilCompareMask(VkCommandBuffer h, VkStencilFaceFlags, uint32_t m) {
    rec(h, [=](Device& d) { d.st.stencilCompareMask = m; d.st.dirtyDynamic = true; });
}
VKAPI_ATTR void VKAPI_CALL CmdSetStencilWriteMask(VkCommandBuffer h, VkStencilFaceFlags, uint32_t m) {
    rec(h, [=](Device& d) { d.st.stencilWriteMask = m; d.st.dirtyDynamic = true; });
}
VKAPI_ATTR void VKAPI_CALL CmdSetStencilReference(VkCommandBuffer h, VkStencilFaceFlags, uint32_t r) {
    rec(h, [=](Device& d) { d.st.stencilRef = r; d.st.dirtyDynamic = true; });
}
VKAPI_ATTR void VKAPI_CALL CmdBindDescriptorSets(VkCommandBuffer h, VkPipelineBindPoint bp, VkPipelineLayout, uint32_t first,
                                                 uint32_t n, const VkDescriptorSet* sets, uint32_t ndyn, const uint32_t* dyn) {
    std::vector<DescriptorSet*> list;
    for (uint32_t i = 0; i < n; i++) list.push_back(from<DescriptorSet>(sets[i]));
    std::vector<uint32_t> offs(dyn, dyn + ndyn);
    rec(h, [=](Device& d) {
        int b = bp == VK_PIPELINE_BIND_POINT_COMPUTE ? 1 : 0;
        size_t cursor = 0;
        for (size_t i = 0; i < list.size() && first + i < 8; i++) {
            d.st.sets[b][first + i] = list[i];
            auto& dst = d.st.dynOffsets[b][first + i];
            dst.clear();
            uint32_t want = list[i] ? list[i]->layout->dynamicCount : 0;
            for (uint32_t k = 0; k < want && cursor < offs.size(); k++) dst.push_back(offs[cursor++]);
        }
        d.st.dirtyDesc[b] = true;
    });
}
VKAPI_ATTR void VKAPI_CALL CmdBindIndexBuffer(VkCommandBuffer h, VkBuffer b, VkDeviceSize off, VkIndexType type) {
    Buffer* buf = from<Buffer>(b);
    rec(h, [=](Device& d) { d.st.ib = buf; d.st.ibOffset = off; d.st.ibType = type; d.st.dirtyIB = true; });
}
VKAPI_ATTR void VKAPI_CALL CmdBindVertexBuffers(VkCommandBuffer h, uint32_t first, uint32_t n, const VkBuffer* bufs,
                                                const VkDeviceSize* offs) {
    std::vector<std::pair<Buffer*, VkDeviceSize>> list;
    for (uint32_t i = 0; i < n; i++) list.emplace_back(from<Buffer>(bufs[i]), offs[i]);
    rec(h, [=](Device& d) {
        for (size_t i = 0; i < list.size() && first + i < 32; i++) {
            d.st.vb[first + i] = list[i].first;
            d.st.vbOffset[first + i] = list[i].second;
        }
        d.st.dirtyVB = true;
    });
}
VKAPI_ATTR void VKAPI_CALL CmdPushConstants(VkCommandBuffer h, VkPipelineLayout, VkShaderStageFlags, uint32_t off,
                                            uint32_t size, const void* data) {
    std::vector<uint8_t> bytes((const uint8_t*)data, (const uint8_t*)data + size);
    rec(h, [=](Device& d) {
        if (off + bytes.size() > sizeof(d.st.push)) return;
        memcpy(d.st.push + off, bytes.data(), bytes.size());
        d.st.dirtyPush = true;
    });
}

// ---------------------------------------------------------------------------
// Draws / dispatch
VKAPI_ATTR void VKAPI_CALL CmdDraw(VkCommandBuffer h, uint32_t vc, uint32_t ic, uint32_t fv, uint32_t fi) {
    rec(h, [=](Device& d) { draw(d, vc, ic, fv, fi); });
}
VKAPI_ATTR void VKAPI_CALL CmdDrawIndexed(VkCommandBuffer h, uint32_t n, uint32_t ic, uint32_t first, int32_t vo, uint32_t fi) {
    rec(h, [=](Device& d) { drawIndexed(d, n, ic, first, vo, fi); });
}
// Indirect arguments live in the CPU shadow of the buffer, so indirect draws
// are simply expanded into direct draws at replay time.
VKAPI_ATTR void VKAPI_CALL CmdDrawIndirect(VkCommandBuffer h, VkBuffer b, VkDeviceSize off, uint32_t count, uint32_t stride) {
    Buffer* buf = from<Buffer>(b);
    rec(h, [=](Device& d) {
        ensureShadow(d, buf);
        for (uint32_t i = 0; i < count; i++) {
            VkDeviceSize o = off + (VkDeviceSize)i * stride;
            if (!buf->mem || o + sizeof(VkDrawIndirectCommand) > buf->ci.size) break;
            VkDrawIndirectCommand c;
            memcpy(&c, buf->data() + o, sizeof(c));
            draw(d, c.vertexCount, c.instanceCount, c.firstVertex, c.firstInstance);
        }
    });
}
VKAPI_ATTR void VKAPI_CALL CmdDrawIndexedIndirect(VkCommandBuffer h, VkBuffer b, VkDeviceSize off, uint32_t count, uint32_t stride) {
    Buffer* buf = from<Buffer>(b);
    rec(h, [=](Device& d) {
        ensureShadow(d, buf);
        for (uint32_t i = 0; i < count; i++) {
            VkDeviceSize o = off + (VkDeviceSize)i * stride;
            if (!buf->mem || o + sizeof(VkDrawIndexedIndirectCommand) > buf->ci.size) break;
            VkDrawIndexedIndirectCommand c;
            memcpy(&c, buf->data() + o, sizeof(c));
            drawIndexed(d, c.indexCount, c.instanceCount, c.firstIndex, c.vertexOffset, c.firstInstance);
        }
    });
}

static void dispatch(Device& d, uint32_t x, uint32_t y, uint32_t z) {
    ExecState& st = d.st;
    Pipeline* p = st.pipeline[1];
    if (!p || !p->compute || !x || !y || !z) return;
    ID3D11DeviceContext* c = d.ctx;
    c->CSSetShader(p->cs.get(), nullptr, 0);
    ID3D11Buffer* pc = dynamicCB(d, d.pushCB, 256);
    if (st.dirtyPush) { writeCB(d, pc, st.push, 256); st.dirtyPush = false; }
    c->CSSetConstantBuffers(SLOT_PUSH_CONSTANTS, 1, &pc);
    d.pendingUavBuffers.clear();
    bindDescriptors(d, 1);
    c->Dispatch(x, y, z);
    ID3D11UnorderedAccessView* none[MAX_UAV] = {};
    c->CSSetUnorderedAccessViews(0, MAX_UAV, none, nullptr);
    ID3D11ShaderResourceView* noSrv[MAX_SRV] = {};
    c->CSSetShaderResources(0, MAX_SRV, noSrv);
    for (Buffer* b : d.pendingUavBuffers) {
        if (!b->gpuWritten) { b->gpuWritten = true; d.gpuWritten.push_back(b); }
    }
    d.pendingUavBuffers.clear();
    st.dirtyDesc[0] = st.dirtyDesc[1] = true;
}
VKAPI_ATTR void VKAPI_CALL CmdDispatch(VkCommandBuffer h, uint32_t x, uint32_t y, uint32_t z) {
    rec(h, [=](Device& d) { dispatch(d, x, y, z); });
}
VKAPI_ATTR void VKAPI_CALL CmdDispatchIndirect(VkCommandBuffer h, VkBuffer b, VkDeviceSize off) {
    Buffer* buf = from<Buffer>(b);
    rec(h, [=](Device& d) {
        ensureShadow(d, buf);
        if (!buf->mem || off + 12 > buf->ci.size) return;
        uint32_t g[3];
        memcpy(g, buf->data() + off, 12);
        dispatch(d, g[0], g[1], g[2]);
    });
}

// ---------------------------------------------------------------------------
// Transfers
VKAPI_ATTR void VKAPI_CALL CmdCopyBuffer(VkCommandBuffer h, VkBuffer s, VkBuffer t, uint32_t n, const VkBufferCopy* r) {
    Buffer *src = from<Buffer>(s), *dst = from<Buffer>(t);
    std::vector<VkBufferCopy> regions(r, r + n);
    rec(h, [=](Device& d) {
        if (!src->mem || !dst->mem) return;
        ensureShadow(d, src);
        ensureShadow(d, dst);
        for (const VkBufferCopy& c : regions) {
            if (c.srcOffset + c.size > src->ci.size || c.dstOffset + c.size > dst->ci.size) continue;
            memmove(dst->data() + c.dstOffset, src->data() + c.srcOffset, (size_t)c.size);
            wrote(d, dst, c.dstOffset, c.size);
        }
    });
}
VKAPI_ATTR void VKAPI_CALL CmdUpdateBuffer(VkCommandBuffer h, VkBuffer t, VkDeviceSize off, VkDeviceSize size, const void* data) {
    Buffer* dst = from<Buffer>(t);
    std::vector<uint8_t> bytes((const uint8_t*)data, (const uint8_t*)data + size);
    rec(h, [=](Device& d) {
        if (!dst->mem || off + bytes.size() > dst->ci.size) return;
        ensureShadow(d, dst);
        memcpy(dst->data() + off, bytes.data(), bytes.size());
        wrote(d, dst, off, bytes.size());
    });
}
VKAPI_ATTR void VKAPI_CALL CmdFillBuffer(VkCommandBuffer h, VkBuffer t, VkDeviceSize off, VkDeviceSize size, uint32_t value) {
    Buffer* dst = from<Buffer>(t);
    rec(h, [=](Device& d) {
        if (!dst->mem || off >= dst->ci.size) return;
        ensureShadow(d, dst);
        VkDeviceSize len = (size == VK_WHOLE_SIZE || off + size > dst->ci.size) ? dst->ci.size - off : size;
        len &= ~(VkDeviceSize)3;
        uint32_t* p = (uint32_t*)(dst->data() + off);
        for (VkDeviceSize i = 0; i < len / 4; i++) p[i] = value;
        wrote(d, dst, off, len);
    });
}

struct CopyLayout { UINT rowPitch, depthPitch, rowBytes, rows; };
static CopyLayout copyLayout(const FormatInfo* fi, const VkBufferImageCopy& r) {
    uint32_t rowLen = r.bufferRowLength ? r.bufferRowLength : r.imageExtent.width;
    uint32_t imgH = r.bufferImageHeight ? r.bufferImageHeight : r.imageExtent.height;
    UINT blk = fi->block;
    CopyLayout l;
    l.rowPitch = ((rowLen + blk - 1) / blk) * fi->bytes;
    l.depthPitch = l.rowPitch * ((imgH + blk - 1) / blk);
    l.rowBytes = ((r.imageExtent.width + blk - 1) / blk) * fi->bytes;
    l.rows = (r.imageExtent.height + blk - 1) / blk;
    return l;
}
static D3D11_BOX boxOf(const Image* img, VkOffset3D o, VkExtent3D e) {
    UINT blk = img->fmt->block;
    D3D11_BOX b{(UINT)o.x, (UINT)o.y, (UINT)o.z, (UINT)o.x + e.width, (UINT)o.y + e.height, (UINT)o.z + e.depth};
    if (blk > 1) { b.right = (b.right + blk - 1) / blk * blk; b.bottom = (b.bottom + blk - 1) / blk * blk; }
    if (img->ci.imageType == VK_IMAGE_TYPE_1D) { b.top = 0; b.bottom = 1; }
    if (img->ci.imageType != VK_IMAGE_TYPE_3D) { b.front = 0; b.back = 1; }
    return b;
}

VKAPI_ATTR void VKAPI_CALL CmdCopyBufferToImage(VkCommandBuffer h, VkBuffer s, VkImage t, VkImageLayout, uint32_t n,
                                                const VkBufferImageCopy* r) {
    Buffer* src = from<Buffer>(s);
    Image* dst = from<Image>(t);
    std::vector<VkBufferImageCopy> regions(r, r + n);
    rec(h, [=](Device& d) {
        if (!src->mem) return;
        ensureShadow(d, src);
        if (dst->fmt->flags & FMT_DEPTH) { VKON_LOG("buffer->depth image upload is not supported by D3D11"); return; }
        for (const VkBufferImageCopy& c : regions) {
            CopyLayout l = copyLayout(dst->fmt, c);
            D3D11_BOX box = boxOf(dst, c.imageOffset, c.imageExtent);
            for (uint32_t layer = 0; layer < c.imageSubresource.layerCount; layer++) {
                VkDeviceSize off = c.bufferOffset + (VkDeviceSize)layer * l.depthPitch * c.imageExtent.depth;
                if (off >= src->ci.size) break;
                d.ctx->UpdateSubresource(dst->tex.get(),
                                         dst->sub(c.imageSubresource.mipLevel, c.imageSubresource.baseArrayLayer + layer),
                                         &box, src->data() + off, l.rowPitch, l.depthPitch);
            }
        }
    });
}

VKAPI_ATTR void VKAPI_CALL CmdCopyImageToBuffer(VkCommandBuffer h, VkImage s, VkImageLayout, VkBuffer t, uint32_t n,
                                                const VkBufferImageCopy* r) {
    Image* src = from<Image>(s);
    Buffer* dst = from<Buffer>(t);
    std::vector<VkBufferImageCopy> regions(r, r + n);
    rec(h, [=](Device& d) {
        if (!dst->mem) return;
        if (src->ci.imageType != VK_IMAGE_TYPE_2D || src->ci.samples != VK_SAMPLE_COUNT_1_BIT) {
            VKON_LOG("image->buffer copy only supports single-sampled 2D images");
            return;
        }
        ensureShadow(d, dst);
        for (const VkBufferImageCopy& c : regions) {
            CopyLayout l = copyLayout(src->fmt, c);
            D3D11_BOX box = boxOf(src, c.imageOffset, c.imageExtent);
            D3D11_TEXTURE2D_DESC td{};
            td.Width = box.right - box.left;
            td.Height = box.bottom - box.top;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = src->fmt->typeless;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_STAGING;
            td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> staging;
            if (FAILED(d.d3d->CreateTexture2D(&td, nullptr, staging.put()))) continue;
            const bool whole = src->fmt->flags & FMT_DEPTH; // depth copies must cover the whole subresource
            for (uint32_t layer = 0; layer < c.imageSubresource.layerCount; layer++) {
                d.ctx->CopySubresourceRegion(staging.get(), 0, 0, 0, 0, src->tex.get(),
                                             src->sub(c.imageSubresource.mipLevel, c.imageSubresource.baseArrayLayer + layer),
                                             whole ? nullptr : &box);
                D3D11_MAPPED_SUBRESOURCE m;
                if (FAILED(d.ctx->Map(staging.get(), 0, D3D11_MAP_READ, 0, &m))) continue;
                VkDeviceSize base = c.bufferOffset + (VkDeviceSize)layer * l.depthPitch;
                for (UINT row = 0; row < l.rows; row++) {
                    VkDeviceSize o = base + (VkDeviceSize)row * l.rowPitch;
                    if (o + l.rowBytes > dst->ci.size) break;
                    memcpy(dst->data() + o, (const uint8_t*)m.pData + (size_t)row * m.RowPitch, l.rowBytes);
                }
                d.ctx->Unmap(staging.get(), 0);
                wrote(d, dst, base, (VkDeviceSize)l.rows * l.rowPitch);
            }
        }
    });
}

VKAPI_ATTR void VKAPI_CALL CmdCopyImage(VkCommandBuffer h, VkImage s, VkImageLayout, VkImage t, VkImageLayout, uint32_t n,
                                        const VkImageCopy* r) {
    Image *src = from<Image>(s), *dst = from<Image>(t);
    std::vector<VkImageCopy> regions(r, r + n);
    rec(h, [=](Device& d) {
        for (const VkImageCopy& c : regions) {
            D3D11_BOX box = boxOf(src, c.srcOffset, c.extent);
            const bool whole = (src->fmt->flags & FMT_DEPTH) || src->ci.samples != VK_SAMPLE_COUNT_1_BIT;
            for (uint32_t l = 0; l < c.srcSubresource.layerCount; l++)
                d.ctx->CopySubresourceRegion(dst->tex.get(), dst->sub(c.dstSubresource.mipLevel, c.dstSubresource.baseArrayLayer + l),
                                             whole ? 0 : c.dstOffset.x, whole ? 0 : c.dstOffset.y, whole ? 0 : c.dstOffset.z,
                                             src->tex.get(), src->sub(c.srcSubresource.mipLevel, c.srcSubresource.baseArrayLayer + l),
                                             whole ? nullptr : &box);
        }
    });
}

static const char* kBlitHlsl =
    "cbuffer C : register(b0) { float4 r; };\n"
    "struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };\n"
    "O vs(uint id : SV_VertexID) { O o; float2 t = float2((id << 1) & 2, id & 2);\n"
    "  o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = lerp(r.xy, r.zw, t); return o; }\n"
    "Texture2D tex : register(t0); SamplerState smp : register(s0);\n"
    "float4 ps(O i) : SV_Target { return tex.Sample(smp, i.uv); }\n";

static bool initBlit(Device& d) {
    if (d.blitVS && d.blitPS) return true;
    std::vector<uint8_t> vs, ps;
    std::string err;
    if (!compileHLSL(kBlitHlsl, "vs", "vs_5_0", vs, err) || !compileHLSL(kBlitHlsl, "ps", "ps_5_0", ps, err)) {
        VKON_LOG("blit shader compile failed: %s", err.c_str());
        return false;
    }
    d.d3d->CreateVertexShader(vs.data(), vs.size(), nullptr, d.blitVS.put());
    d.d3d->CreatePixelShader(ps.data(), ps.size(), nullptr, d.blitPS.put());
    D3D11_SAMPLER_DESC sd{};
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    sd.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    d.d3d->CreateSamplerState(&sd, d.blitLinear.put());
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    d.d3d->CreateSamplerState(&sd, d.blitPoint.put());
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    d.d3d->CreateRasterizerState(&rd, d.blitRS.put());
    D3D11_DEPTH_STENCIL_DESC dd{};
    d.d3d->CreateDepthStencilState(&dd, d.blitDS.put());
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    d.d3d->CreateBlendState(&bd, d.blitBlend.put());
    return d.blitVS && d.blitPS;
}

VKAPI_ATTR void VKAPI_CALL CmdBlitImage(VkCommandBuffer h, VkImage s, VkImageLayout, VkImage t, VkImageLayout, uint32_t n,
                                        const VkImageBlit* r, VkFilter filter) {
    Image *src = from<Image>(s), *dst = from<Image>(t);
    std::vector<VkImageBlit> regions(r, r + n);
    rec(h, [=](Device& d) {
        if ((src->fmt->flags | dst->fmt->flags) & (FMT_DEPTH | FMT_INT | FMT_COMPRESSED)) {
            VKON_LOG("blit of depth/integer/compressed formats is not supported");
            return;
        }
        if (src->ci.imageType != VK_IMAGE_TYPE_2D || dst->ci.imageType != VK_IMAGE_TYPE_2D || !initBlit(d)) return;
        ID3D11DeviceContext* c = d.ctx;
        for (const VkImageBlit& b : regions) {
            const uint32_t sm = b.srcSubresource.mipLevel, dm = b.dstSubresource.mipLevel;
            const float sw = (float)std::max(1u, src->ci.extent.width >> sm), sh = (float)std::max(1u, src->ci.extent.height >> sm);
            for (uint32_t l = 0; l < b.srcSubresource.layerCount; l++) {
                D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
                sd.Format = src->fmt->srv;
                sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
                sd.Texture2DArray = {sm, 1, b.srcSubresource.baseArrayLayer + l, 1};
                if (src->ci.arrayLayers == 1) { sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sd.Texture2D = {sm, 1}; }
                D3D11_RENDER_TARGET_VIEW_DESC rd{};
                rd.Format = dst->fmt->dxgi;
                rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
                rd.Texture2DArray = {dm, b.dstSubresource.baseArrayLayer + l, 1};
                ComPtr<ID3D11ShaderResourceView> srv;
                ComPtr<ID3D11RenderTargetView> rtv;
                if (FAILED(d.d3d->CreateShaderResourceView(src->tex.get(), &sd, srv.put())) ||
                    FAILED(d.d3d->CreateRenderTargetView(dst->tex.get(), &rd, rtv.put()))) {
                    VKON_LOG("blit: view creation failed");
                    continue;
                }
                // Destination rectangle (may be mirrored); mirror by swapping the source coordinates.
                float u0 = b.srcOffsets[0].x / sw, v0 = b.srcOffsets[0].y / sh, u1 = b.srcOffsets[1].x / sw, v1 = b.srcOffsets[1].y / sh;
                LONG x0 = b.dstOffsets[0].x, y0 = b.dstOffsets[0].y, x1 = b.dstOffsets[1].x, y1 = b.dstOffsets[1].y;
                if (x0 > x1) { std::swap(x0, x1); std::swap(u0, u1); }
                if (y0 > y1) { std::swap(y0, y1); std::swap(v0, v1); }
                float rect[4] = {u0, v0, u1, v1};
                ID3D11Buffer* cb = dynamicCB(d, d.blitCB, 16);
                writeCB(d, cb, rect, sizeof(rect));
                D3D11_VIEWPORT vp{(float)x0, (float)y0, (float)(x1 - x0), (float)(y1 - y0), 0, 1};
                D3D11_RECT sc{x0, y0, x1, y1};
                ID3D11RenderTargetView* rt = rtv.get();
                ID3D11ShaderResourceView* sr = srv.get();
                ID3D11SamplerState* sp = filter == VK_FILTER_LINEAR ? d.blitLinear.get() : d.blitPoint.get();
                c->OMSetRenderTargets(1, &rt, nullptr);
                c->OMSetBlendState(d.blitBlend.get(), nullptr, 0xffffffff);
                c->OMSetDepthStencilState(d.blitDS.get(), 0);
                c->RSSetState(d.blitRS.get());
                c->RSSetViewports(1, &vp);
                c->RSSetScissorRects(1, &sc);
                c->IASetInputLayout(nullptr);
                c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                c->VSSetShader(d.blitVS.get(), nullptr, 0);
                c->PSSetShader(d.blitPS.get(), nullptr, 0);
                c->GSSetShader(nullptr, nullptr, 0);
                c->VSSetConstantBuffers(0, 1, &cb);
                c->PSSetShaderResources(0, 1, &sr);
                c->PSSetSamplers(0, 1, &sp);
                c->Draw(3, 0);
                sr = nullptr;
                c->PSSetShaderResources(0, 1, &sr);
                c->OMSetRenderTargets(0, nullptr, nullptr);
            }
        }
        d.st.invalidate();
    });
}

VKAPI_ATTR void VKAPI_CALL CmdResolveImage(VkCommandBuffer h, VkImage s, VkImageLayout, VkImage t, VkImageLayout, uint32_t n,
                                           const VkImageResolve* r) {
    Image *src = from<Image>(s), *dst = from<Image>(t);
    std::vector<VkImageResolve> regions(r, r + n);
    rec(h, [=](Device& d) {
        for (const VkImageResolve& c : regions)
            for (uint32_t l = 0; l < c.srcSubresource.layerCount; l++)
                d.ctx->ResolveSubresource(dst->tex.get(), dst->sub(c.dstSubresource.mipLevel, c.dstSubresource.baseArrayLayer + l),
                                          src->tex.get(), src->sub(c.srcSubresource.mipLevel, c.srcSubresource.baseArrayLayer + l),
                                          dst->fmt->dxgi);
    });
}

VKAPI_ATTR void VKAPI_CALL CmdClearColorImage(VkCommandBuffer h, VkImage t, VkImageLayout, const VkClearColorValue* col,
                                              uint32_t n, const VkImageSubresourceRange* r) {
    Image* img = from<Image>(t);
    VkClearColorValue cv = *col;
    std::vector<VkImageSubresourceRange> ranges(r, r + n);
    rec(h, [=](Device& d) {
        float f[4];
        for (int i = 0; i < 4; i++) f[i] = (img->fmt->flags & FMT_INT) ? (float)cv.int32[i] : cv.float32[i];
        for (const auto& rg : ranges) {
            uint32_t levels = rg.levelCount == VK_REMAINING_MIP_LEVELS ? img->ci.mipLevels - rg.baseMipLevel : rg.levelCount;
            uint32_t layers = rg.layerCount == VK_REMAINING_ARRAY_LAYERS ? img->ci.arrayLayers - rg.baseArrayLayer : rg.layerCount;
            for (uint32_t m = 0; m < levels; m++) {
                D3D11_RENDER_TARGET_VIEW_DESC rd{};
                rd.Format = img->fmt->dxgi;
                if (img->ci.imageType == VK_IMAGE_TYPE_3D) {
                    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE3D;
                    rd.Texture3D = {rg.baseMipLevel + m, 0, (UINT)-1};
                } else if (img->ci.imageType == VK_IMAGE_TYPE_1D) {
                    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE1DARRAY;
                    rd.Texture1DArray = {rg.baseMipLevel + m, rg.baseArrayLayer, layers};
                } else {
                    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
                    rd.Texture2DArray = {rg.baseMipLevel + m, rg.baseArrayLayer, layers};
                }
                ComPtr<ID3D11RenderTargetView> rtv;
                if (SUCCEEDED(d.d3d->CreateRenderTargetView(img->tex.get(), &rd, rtv.put()))) d.ctx->ClearRenderTargetView(rtv.get(), f);
                else VKON_LOG("vkCmdClearColorImage: image is not clearable (format %d)", img->ci.format);
            }
        }
    });
}

VKAPI_ATTR void VKAPI_CALL CmdClearDepthStencilImage(VkCommandBuffer h, VkImage t, VkImageLayout,
                                                     const VkClearDepthStencilValue* val, uint32_t n,
                                                     const VkImageSubresourceRange* r) {
    Image* img = from<Image>(t);
    VkClearDepthStencilValue cv = *val;
    std::vector<VkImageSubresourceRange> ranges(r, r + n);
    rec(h, [=](Device& d) {
        for (const auto& rg : ranges) {
            uint32_t levels = rg.levelCount == VK_REMAINING_MIP_LEVELS ? img->ci.mipLevels - rg.baseMipLevel : rg.levelCount;
            uint32_t layers = rg.layerCount == VK_REMAINING_ARRAY_LAYERS ? img->ci.arrayLayers - rg.baseArrayLayer : rg.layerCount;
            UINT flags = 0;
            if (rg.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) flags |= D3D11_CLEAR_DEPTH;
            if ((rg.aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) && (img->fmt->flags & FMT_STENCIL)) flags |= D3D11_CLEAR_STENCIL;
            for (uint32_t m = 0; m < levels; m++) {
                D3D11_DEPTH_STENCIL_VIEW_DESC dd{};
                dd.Format = img->fmt->dxgi;
                dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
                dd.Texture2DArray = {rg.baseMipLevel + m, rg.baseArrayLayer, layers};
                ComPtr<ID3D11DepthStencilView> dsv;
                if (SUCCEEDED(d.d3d->CreateDepthStencilView(img->tex.get(), &dd, dsv.put())))
                    d.ctx->ClearDepthStencilView(dsv.get(), flags, cv.depth, (UINT8)cv.stencil);
            }
        }
    });
}

VKAPI_ATTR void VKAPI_CALL CmdClearAttachments(VkCommandBuffer h, uint32_t na, const VkClearAttachment* atts, uint32_t nr,
                                               const VkClearRect* rects) {
    std::vector<VkClearAttachment> a(atts, atts + na);
    std::vector<VkClearRect> r(rects, rects + nr);
    rec(h, [=](Device& d) {
        ExecState& st = d.st;
        if (!st.rp) return;
        const SubpassData& sp = st.rp->subpasses[st.subpass];
        std::vector<D3D11_RECT> dr;
        for (const VkClearRect& cr : r) dr.push_back(toRect(cr.rect));
        for (const VkClearAttachment& ca : a) {
            if (ca.aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) {
                if (ca.colorAttachment >= sp.color.size() || sp.color[ca.colorAttachment].attachment == VK_ATTACHMENT_UNUSED) continue;
                clearRects(d, st.fb->views[sp.color[ca.colorAttachment].attachment], ca.clearValue, 0, dr.data(), (UINT)dr.size());
            } else if (sp.depth.attachment != VK_ATTACHMENT_UNUSED) {
                ImageView* v = st.fb->views[sp.depth.attachment];
                UINT flags = 0;
                if (ca.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) flags |= D3D11_CLEAR_DEPTH;
                if ((ca.aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) && (v->fmt->flags & FMT_STENCIL)) flags |= D3D11_CLEAR_STENCIL;
                if (flags) clearRects(d, v, ca.clearValue, flags, dr.data(), (UINT)dr.size());
            }
        }
        setSubpassTargets(d);
    });
}

// ---------------------------------------------------------------------------
// Barriers and events have nothing to do on D3D11 (it tracks hazards itself).
VKAPI_ATTR void VKAPI_CALL CmdPipelineBarrier(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags, VkDependencyFlags,
                                              uint32_t, const VkMemoryBarrier*, uint32_t, const VkBufferMemoryBarrier*, uint32_t,
                                              const VkImageMemoryBarrier*) {}
VKAPI_ATTR void VKAPI_CALL CmdSetEvent(VkCommandBuffer h, VkEvent e, VkPipelineStageFlags) {
    Event* ev = from<Event>(e);
    rec(h, [=](Device&) { ev->set = true; });
}
VKAPI_ATTR void VKAPI_CALL CmdResetEvent(VkCommandBuffer h, VkEvent e, VkPipelineStageFlags) {
    Event* ev = from<Event>(e);
    rec(h, [=](Device&) { ev->set = false; });
}
VKAPI_ATTR void VKAPI_CALL CmdWaitEvents(VkCommandBuffer, uint32_t, const VkEvent*, VkPipelineStageFlags, VkPipelineStageFlags,
                                         uint32_t, const VkMemoryBarrier*, uint32_t, const VkBufferMemoryBarrier*, uint32_t,
                                         const VkImageMemoryBarrier*) {}

// ---------------------------------------------------------------------------
// Queries
VKAPI_ATTR void VKAPI_CALL CmdBeginQuery(VkCommandBuffer h, VkQueryPool ph, uint32_t q, VkQueryControlFlags) {
    QueryPool* p = from<QueryPool>(ph);
    rec(h, [=](Device& d) {
        if (q < p->queries.size() && p->queries[q]) d.ctx->Begin(p->queries[q].get());
    });
}
VKAPI_ATTR void VKAPI_CALL CmdEndQuery(VkCommandBuffer h, VkQueryPool ph, uint32_t q) {
    QueryPool* p = from<QueryPool>(ph);
    rec(h, [=](Device& d) {
        if (q < p->queries.size() && p->queries[q]) { d.ctx->End(p->queries[q].get()); p->active[q] = 1; }
    });
}
VKAPI_ATTR void VKAPI_CALL CmdResetQueryPool(VkCommandBuffer h, VkQueryPool ph, uint32_t first, uint32_t n) {
    QueryPool* p = from<QueryPool>(ph);
    rec(h, [=](Device&) {
        for (uint32_t i = first; i < first + n && i < p->active.size(); i++) p->active[i] = 0;
    });
}
VKAPI_ATTR void VKAPI_CALL CmdWriteTimestamp(VkCommandBuffer, VkPipelineStageFlagBits, VkQueryPool, uint32_t) {}
VKAPI_ATTR void VKAPI_CALL CmdCopyQueryPoolResults(VkCommandBuffer h, VkQueryPool ph, uint32_t first, uint32_t n, VkBuffer b,
                                                   VkDeviceSize off, VkDeviceSize stride, VkQueryResultFlags flags) {
    QueryPool* p = from<QueryPool>(ph);
    Buffer* buf = from<Buffer>(b);
    rec(h, [=](Device& d) {
        if (!buf->mem) return;
        ensureShadow(d, buf);
        for (uint32_t i = 0; i < n; i++) {
            uint64_t value = 0;
            ID3D11Query* q = first + i < p->queries.size() ? p->queries[first + i].get() : nullptr;
            if (q && p->active[first + i])
                for (int tries = 0; tries < 2000 && d.ctx->GetData(q, &value, sizeof(value), 0) != S_OK; tries++) Sleep(1);
            VkDeviceSize o = off + i * stride;
            size_t sz = (flags & VK_QUERY_RESULT_64_BIT) ? 8 : 4;
            if (o + sz > buf->ci.size) break;
            memcpy(buf->data() + o, &value, sz);
            wrote(d, buf, o, sz);
        }
    });
}

} // namespace vkon11
