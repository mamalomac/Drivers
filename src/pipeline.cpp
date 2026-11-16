// Shader modules, descriptor sets, pipeline layouts, render passes, pipelines.
#include "vkon11.h"

namespace vkon11 {

// ---------------------------------------------------------------------------
VKAPI_ATTR VkResult VKAPI_CALL CreateShaderModule(VkDevice, const VkShaderModuleCreateInfo* ci,
                                                  const VkAllocationCallbacks*, VkShaderModule* out) {
    ShaderModule* m = new ShaderModule();
    m->code.assign(ci->pCode, ci->pCode + ci->codeSize / 4);
    *out = to<VkShaderModule>(m);
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL DestroyShaderModule(VkDevice, VkShaderModule h, const VkAllocationCallbacks*) {
    delete from<ShaderModule>(h);
}

VKAPI_ATTR VkResult VKAPI_CALL CreatePipelineCache(VkDevice, const VkPipelineCacheCreateInfo*,
                                                   const VkAllocationCallbacks*, VkPipelineCache* out) {
    *out = to<VkPipelineCache>(new int(0));
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL DestroyPipelineCache(VkDevice, VkPipelineCache h, const VkAllocationCallbacks*) {
    delete from<int>(h);
}
VKAPI_ATTR VkResult VKAPI_CALL GetPipelineCacheData(VkDevice, VkPipelineCache, size_t* size, void*) {
    *size = 0;
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL MergePipelineCaches(VkDevice, VkPipelineCache, uint32_t, const VkPipelineCache*) {
    return VK_SUCCESS;
}

// ---------------------------------------------------------------------------
// Descriptors
static bool isDynamic(VkDescriptorType t) {
    return t == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || t == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateDescriptorSetLayout(VkDevice, const VkDescriptorSetLayoutCreateInfo* ci,
                                                         const VkAllocationCallbacks*, VkDescriptorSetLayout* out) {
    auto data = std::make_shared<SetLayoutData>();
    for (uint32_t i = 0; i < ci->bindingCount; i++) {
        const VkDescriptorSetLayoutBinding& s = ci->pBindings[i];
        SetBinding b;
        b.binding = s.binding;
        b.type = s.descriptorType;
        b.count = s.descriptorCount;
        b.stages = s.stageFlags;
        if (s.pImmutableSamplers && (b.type == VK_DESCRIPTOR_TYPE_SAMPLER || b.type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER))
            for (uint32_t j = 0; j < s.descriptorCount; j++) b.immutableSamplers.push_back(from<Sampler>(s.pImmutableSamplers[j]));
        data->bindings.push_back(std::move(b));
    }
    std::sort(data->bindings.begin(), data->bindings.end(),
              [](const SetBinding& a, const SetBinding& b) { return a.binding < b.binding; });
    for (SetBinding& b : data->bindings) {
        b.offset = data->descriptorCount;
        data->descriptorCount += b.count;
        if (isDynamic(b.type)) { b.dynIndex = data->dynamicCount; data->dynamicCount += b.count; }
    }
    auto* l = new DescriptorSetLayout();
    l->data = data;
    *out = to<VkDescriptorSetLayout>(l);
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL DestroyDescriptorSetLayout(VkDevice, VkDescriptorSetLayout h, const VkAllocationCallbacks*) {
    delete from<DescriptorSetLayout>(h);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateDescriptorPool(VkDevice, const VkDescriptorPoolCreateInfo*,
                                                    const VkAllocationCallbacks*, VkDescriptorPool* out) {
    *out = to<VkDescriptorPool>(new DescriptorPool());
    return VK_SUCCESS;
}
static void freeSets(DescriptorPool* p) {
    for (DescriptorSet* s : p->sets) delete s;
    p->sets.clear();
}
VKAPI_ATTR void VKAPI_CALL DestroyDescriptorPool(VkDevice, VkDescriptorPool h, const VkAllocationCallbacks*) {
    DescriptorPool* p = from<DescriptorPool>(h);
    if (!p) return;
    freeSets(p);
    delete p;
}
VKAPI_ATTR VkResult VKAPI_CALL ResetDescriptorPool(VkDevice, VkDescriptorPool h, VkDescriptorPoolResetFlags) {
    freeSets(from<DescriptorPool>(h));
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL AllocateDescriptorSets(VkDevice, const VkDescriptorSetAllocateInfo* ai,
                                                      VkDescriptorSet* out) {
    DescriptorPool* pool = from<DescriptorPool>(ai->descriptorPool);
    for (uint32_t i = 0; i < ai->descriptorSetCount; i++) {
        DescriptorSet* s = new DescriptorSet();
        s->layout = from<DescriptorSetLayout>(ai->pSetLayouts[i])->data;
        s->desc.resize(s->layout->descriptorCount);
        s->pool = pool;
        for (const SetBinding& b : s->layout->bindings)
            for (size_t j = 0; j < b.immutableSamplers.size(); j++) s->desc[b.offset + j].sampler = b.immutableSamplers[j];
        pool->sets.push_back(s);
        out[i] = to<VkDescriptorSet>(s);
    }
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL FreeDescriptorSets(VkDevice, VkDescriptorPool h, uint32_t n, const VkDescriptorSet* sets) {
    DescriptorPool* pool = from<DescriptorPool>(h);
    for (uint32_t i = 0; i < n; i++) {
        DescriptorSet* s = from<DescriptorSet>(sets[i]);
        if (!s) continue;
        pool->sets.erase(std::remove(pool->sets.begin(), pool->sets.end(), s), pool->sets.end());
        delete s;
    }
    return VK_SUCCESS;
}

static const SetBinding* findBinding(const SetLayoutData& l, uint32_t binding) {
    for (const SetBinding& b : l.bindings)
        if (b.binding == binding) return &b;
    return nullptr;
}

VKAPI_ATTR void VKAPI_CALL UpdateDescriptorSets(VkDevice, uint32_t nw, const VkWriteDescriptorSet* writes, uint32_t nc,
                                                const VkCopyDescriptorSet* copies) {
    for (uint32_t i = 0; i < nw; i++) {
        const VkWriteDescriptorSet& w = writes[i];
        DescriptorSet* s = from<DescriptorSet>(w.dstSet);
        const SetBinding* b = findBinding(*s->layout, w.dstBinding);
        if (!b) continue;
        for (uint32_t j = 0; j < w.descriptorCount; j++) {
            size_t idx = b->offset + w.dstArrayElement + j;
            if (idx >= s->desc.size()) break;
            Descriptor& d = s->desc[idx];
            switch (w.descriptorType) {
            case VK_DESCRIPTOR_TYPE_SAMPLER:
                if (b->immutableSamplers.empty()) d.sampler = from<Sampler>(w.pImageInfo[j].sampler);
                break;
            case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
                if (b->immutableSamplers.empty()) d.sampler = from<Sampler>(w.pImageInfo[j].sampler);
                d.view = from<ImageView>(w.pImageInfo[j].imageView);
                break;
            case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
            case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
            case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
                d.view = from<ImageView>(w.pImageInfo[j].imageView);
                break;
            case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
                d.bufferView = from<BufferView>(w.pTexelBufferView[j]);
                break;
            default:
                d.buffer = from<Buffer>(w.pBufferInfo[j].buffer);
                d.offset = w.pBufferInfo[j].offset;
                d.range = w.pBufferInfo[j].range;
                break;
            }
        }
    }
    for (uint32_t i = 0; i < nc; i++) {
        const VkCopyDescriptorSet& c = copies[i];
        DescriptorSet *src = from<DescriptorSet>(c.srcSet), *dst = from<DescriptorSet>(c.dstSet);
        const SetBinding *sb = findBinding(*src->layout, c.srcBinding), *db = findBinding(*dst->layout, c.dstBinding);
        if (!sb || !db) continue;
        for (uint32_t j = 0; j < c.descriptorCount; j++) {
            size_t si = sb->offset + c.srcArrayElement + j, di = db->offset + c.dstArrayElement + j;
            if (si < src->desc.size() && di < dst->desc.size()) dst->desc[di] = src->desc[si];
        }
    }
}

VKAPI_ATTR VkResult VKAPI_CALL CreatePipelineLayout(VkDevice, const VkPipelineLayoutCreateInfo* ci,
                                                    const VkAllocationCallbacks*, VkPipelineLayout* out) {
    auto data = std::make_shared<PipelineLayoutData>();
    for (uint32_t i = 0; i < ci->setLayoutCount; i++) data->sets.push_back(from<DescriptorSetLayout>(ci->pSetLayouts[i])->data);
    static const VkShaderStageFlags stageBit[ST_COUNT] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT,
                                                          VK_SHADER_STAGE_COMPUTE_BIT};
    for (int st = 0; st < ST_COUNT; st++) {
        for (uint32_t i = 0; i < data->sets.size(); i++) {
            const auto& bindings = data->sets[i]->bindings;
            for (uint32_t j = 0; j < bindings.size(); j++) {
                const SetBinding& b = bindings[j];
                if (!(b.stages & stageBit[st])) continue;
                SlotMap s;
                s.set = i;
                s.bindingIndex = j;
                switch (b.type) {
                case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
                case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
                    s.cbv = data->cbvCount[st]; data->cbvCount[st] += b.count; break;
                case VK_DESCRIPTOR_TYPE_SAMPLER:
                    s.sampler = data->samplerCount[st]; data->samplerCount[st] += b.count; break;
                case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
                    s.sampler = data->samplerCount[st]; data->samplerCount[st] += b.count;
                    s.srv = data->srvCount[st]; data->srvCount[st] += b.count; break;
                case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
                case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
                case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
                    s.srv = data->srvCount[st]; data->srvCount[st] += b.count; break;
                default:
                    s.uav = data->uavCount[st]; data->uavCount[st] += b.count; break;
                }
                data->slots[st].push_back(s);
            }
        }
    }
    auto* l = new PipelineLayout();
    l->data = data;
    *out = to<VkPipelineLayout>(l);
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL DestroyPipelineLayout(VkDevice, VkPipelineLayout h, const VkAllocationCallbacks*) {
    delete from<PipelineLayout>(h);
}

// ---------------------------------------------------------------------------
// Render passes / framebuffers
VKAPI_ATTR VkResult VKAPI_CALL CreateRenderPass(VkDevice, const VkRenderPassCreateInfo* ci, const VkAllocationCallbacks*,
                                                VkRenderPass* out) {
    RenderPass* rp = new RenderPass();
    rp->attachments.assign(ci->pAttachments, ci->pAttachments + ci->attachmentCount);
    for (uint32_t i = 0; i < ci->subpassCount; i++) {
        const VkSubpassDescription& s = ci->pSubpasses[i];
        SubpassData sd;
        sd.color.assign(s.pColorAttachments, s.pColorAttachments + s.colorAttachmentCount);
        if (s.pResolveAttachments) sd.resolve.assign(s.pResolveAttachments, s.pResolveAttachments + s.colorAttachmentCount);
        sd.input.assign(s.pInputAttachments, s.pInputAttachments + s.inputAttachmentCount);
        if (s.pDepthStencilAttachment) sd.depth = *s.pDepthStencilAttachment;
        rp->subpasses.push_back(std::move(sd));
    }
    *out = to<VkRenderPass>(rp);
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL DestroyRenderPass(VkDevice, VkRenderPass h, const VkAllocationCallbacks*) {
    delete from<RenderPass>(h);
}
VKAPI_ATTR void VKAPI_CALL GetRenderAreaGranularity(VkDevice, VkRenderPass, VkExtent2D* g) { *g = {1, 1}; }

VKAPI_ATTR VkResult VKAPI_CALL CreateFramebuffer(VkDevice, const VkFramebufferCreateInfo* ci,
                                                 const VkAllocationCallbacks*, VkFramebuffer* out) {
    Framebuffer* fb = new Framebuffer();
    for (uint32_t i = 0; i < ci->attachmentCount; i++) fb->views.push_back(from<ImageView>(ci->pAttachments[i]));
    fb->width = ci->width;
    fb->height = ci->height;
    fb->layers = ci->layers;
    *out = to<VkFramebuffer>(fb);
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL DestroyFramebuffer(VkDevice, VkFramebuffer h, const VkAllocationCallbacks*) {
    delete from<Framebuffer>(h);
}

// ---------------------------------------------------------------------------
// Pipelines
static D3D11_BLEND blendFactor(VkBlendFactor f, bool alpha) {
    switch (f) {
    case VK_BLEND_FACTOR_ZERO: return D3D11_BLEND_ZERO;
    case VK_BLEND_FACTOR_ONE: return D3D11_BLEND_ONE;
    case VK_BLEND_FACTOR_SRC_COLOR: return alpha ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR;
    case VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR: return alpha ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_SRC_COLOR;
    case VK_BLEND_FACTOR_DST_COLOR: return alpha ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR;
    case VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR: return alpha ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_DEST_COLOR;
    case VK_BLEND_FACTOR_SRC_ALPHA: return D3D11_BLEND_SRC_ALPHA;
    case VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA: return D3D11_BLEND_INV_SRC_ALPHA;
    case VK_BLEND_FACTOR_DST_ALPHA: return D3D11_BLEND_DEST_ALPHA;
    case VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA: return D3D11_BLEND_INV_DEST_ALPHA;
    case VK_BLEND_FACTOR_CONSTANT_COLOR:
    case VK_BLEND_FACTOR_CONSTANT_ALPHA: return D3D11_BLEND_BLEND_FACTOR;
    case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR:
    case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA: return D3D11_BLEND_INV_BLEND_FACTOR;
    case VK_BLEND_FACTOR_SRC_ALPHA_SATURATE: return alpha ? D3D11_BLEND_ONE : D3D11_BLEND_SRC_ALPHA_SAT;
    case VK_BLEND_FACTOR_SRC1_COLOR: return alpha ? D3D11_BLEND_SRC1_ALPHA : D3D11_BLEND_SRC1_COLOR;
    case VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR: return alpha ? D3D11_BLEND_INV_SRC1_ALPHA : D3D11_BLEND_INV_SRC1_COLOR;
    case VK_BLEND_FACTOR_SRC1_ALPHA: return D3D11_BLEND_SRC1_ALPHA;
    case VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA: return D3D11_BLEND_INV_SRC1_ALPHA;
    default: return D3D11_BLEND_ONE;
    }
}

static D3D11_PRIMITIVE_TOPOLOGY topology(VkPrimitiveTopology t, uint32_t patchPoints) {
    switch (t) {
    case VK_PRIMITIVE_TOPOLOGY_POINT_LIST: return D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
    case VK_PRIMITIVE_TOPOLOGY_LINE_LIST: return D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
    case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP: return D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST: return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP: return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
    case VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY: return D3D11_PRIMITIVE_TOPOLOGY_LINELIST_ADJ;
    case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY: return D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ;
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY: return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ;
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY: return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ;
    case VK_PRIMITIVE_TOPOLOGY_PATCH_LIST:
        return (D3D11_PRIMITIVE_TOPOLOGY)(D3D11_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST + std::max(1u, patchPoints) - 1);
    default: return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST; // triangle fans have no D3D11 equivalent
    }
}

static void logShaderError(const char* what, const std::string& err, const std::string& hlsl) {
    VKON_LOG("%s failed: %s", what, err.c_str());
    char dump[8];
    if (GetEnvironmentVariableA("VKON11_DUMP_HLSL", dump, sizeof(dump))) VKON_LOG("---- HLSL ----\n%s\n--------------", hlsl.c_str());
}

static VkResult createGraphicsPipeline(Device& d, const VkGraphicsPipelineCreateInfo& ci, Pipeline** out) {
    auto p = std::make_unique<Pipeline>();
    p->layout = from<PipelineLayout>(ci.layout)->data;

    const VkPipelineShaderStageCreateInfo *vsInfo = nullptr, *psInfo = nullptr;
    for (uint32_t i = 0; i < ci.stageCount; i++) {
        if (ci.pStages[i].stage == VK_SHADER_STAGE_VERTEX_BIT) vsInfo = &ci.pStages[i];
        else if (ci.pStages[i].stage == VK_SHADER_STAGE_FRAGMENT_BIT) psInfo = &ci.pStages[i];
        else {
            VKON_LOG("unsupported shader stage 0x%x (geometry/tessellation are not implemented)", ci.pStages[i].stage);
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }
    }
    if (!vsInfo) return VK_ERROR_INITIALIZATION_FAILED;

    std::string vsHlsl, psHlsl, err;
    std::vector<uint8_t> vsCode, psCode;
    if (!translateShader(*from<ShaderModule>(vsInfo->module), vsInfo->pName, VK_SHADER_STAGE_VERTEX_BIT, *p->layout,
                         vsInfo->pSpecializationInfo, vsHlsl, err)) {
        logShaderError("vertex shader translation", err, "");
        return VK_ERROR_INVALID_SHADER_NV;
    }
    if (!compileHLSL(vsHlsl, "main", "vs_5_0", vsCode, err)) {
        logShaderError("vertex shader compile", err, vsHlsl);
        return VK_ERROR_INVALID_SHADER_NV;
    }
    p->usesVertexInfo = vsHlsl.find("SPIRV_Cross_BaseVertex") != std::string::npos;
    if (FAILED(d.d3d->CreateVertexShader(vsCode.data(), vsCode.size(), nullptr, p->vs.put()))) return VK_ERROR_INVALID_SHADER_NV;

    const bool discard = ci.pRasterizationState && ci.pRasterizationState->rasterizerDiscardEnable;
    p->rasterDiscard = discard;
    if (psInfo && !discard) {
        if (!translateShader(*from<ShaderModule>(psInfo->module), psInfo->pName, VK_SHADER_STAGE_FRAGMENT_BIT,
                             *p->layout, psInfo->pSpecializationInfo, psHlsl, err)) {
            logShaderError("fragment shader translation", err, "");
            return VK_ERROR_INVALID_SHADER_NV;
        }
        matchStageInterface(vsHlsl, psHlsl);
        if (!compileHLSL(psHlsl, "main", "ps_5_0", psCode, err)) {
            logShaderError("fragment shader compile", err, psHlsl);
            return VK_ERROR_INVALID_SHADER_NV;
        }
        if (FAILED(d.d3d->CreatePixelShader(psCode.data(), psCode.size(), nullptr, p->ps.put()))) return VK_ERROR_INVALID_SHADER_NV;
    }

    // Vertex input
    if (const auto* vi = ci.pVertexInputState) {
        std::vector<D3D11_INPUT_ELEMENT_DESC> elems;
        for (uint32_t i = 0; i < vi->vertexBindingDescriptionCount; i++) {
            const auto& b = vi->pVertexBindingDescriptions[i];
            if (b.binding < 32) { p->strides[b.binding] = b.stride; p->vbMask |= 1u << b.binding; }
        }
        for (uint32_t i = 0; i < vi->vertexAttributeDescriptionCount; i++) {
            const auto& a = vi->pVertexAttributeDescriptions[i];
            const FormatInfo* fi = findFormat(a.format);
            if (!fi) { VKON_LOG("unsupported vertex format %d", a.format); return VK_ERROR_FORMAT_NOT_SUPPORTED; }
            D3D11_INPUT_ELEMENT_DESC e{};
            e.SemanticName = "TEXCOORD";
            e.SemanticIndex = a.location;
            e.Format = fi->dxgi;
            e.InputSlot = a.binding;
            e.AlignedByteOffset = a.offset;
            e.InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
            for (uint32_t j = 0; j < vi->vertexBindingDescriptionCount; j++) {
                const auto& b = vi->pVertexBindingDescriptions[j];
                if (b.binding == a.binding && b.inputRate == VK_VERTEX_INPUT_RATE_INSTANCE) {
                    e.InputSlotClass = D3D11_INPUT_PER_INSTANCE_DATA;
                    e.InstanceDataStepRate = 1;
                }
            }
            elems.push_back(e);
        }
        if (!elems.empty()) {
            HRESULT hr = d.d3d->CreateInputLayout(elems.data(), (UINT)elems.size(), vsCode.data(), vsCode.size(), p->inputLayout.put());
            if (FAILED(hr)) { VKON_LOG("CreateInputLayout failed hr=0x%08lx", (unsigned long)hr); return VK_ERROR_INITIALIZATION_FAILED; }
        }
    }

    if (ci.pInputAssemblyState)
        p->topology = topology(ci.pInputAssemblyState->topology, ci.pTessellationState ? ci.pTessellationState->patchControlPoints : 3);

    const VkSampleCountFlagBits samples = (ci.pMultisampleState && !discard) ? ci.pMultisampleState->rasterizationSamples : VK_SAMPLE_COUNT_1_BIT;

    // Rasterizer
    D3D11_RASTERIZER_DESC& rs = p->rsDesc;
    rs.FillMode = D3D11_FILL_SOLID;
    rs.CullMode = D3D11_CULL_NONE;
    rs.DepthClipEnable = TRUE;
    rs.ScissorEnable = TRUE;
    if (const auto* r = ci.pRasterizationState) {
        if (r->polygonMode == VK_POLYGON_MODE_LINE) rs.FillMode = D3D11_FILL_WIREFRAME;
        if (r->cullMode == VK_CULL_MODE_FRONT_BIT) rs.CullMode = D3D11_CULL_FRONT;
        else if (r->cullMode == VK_CULL_MODE_BACK_BIT) rs.CullMode = D3D11_CULL_BACK;
        else if (r->cullMode == VK_CULL_MODE_FRONT_AND_BACK) p->rasterDiscard = true;
        rs.FrontCounterClockwise = r->frontFace == VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.DepthClipEnable = !r->depthClampEnable;
        if (r->depthBiasEnable) {
            rs.DepthBias = (INT)r->depthBiasConstantFactor;
            rs.DepthBiasClamp = r->depthBiasClamp;
            rs.SlopeScaledDepthBias = r->depthBiasSlopeFactor;
        }
    }
    rs.MultisampleEnable = samples != VK_SAMPLE_COUNT_1_BIT;

    // Depth/stencil
    D3D11_DEPTH_STENCIL_DESC& ds = p->dsDesc;
    ds.DepthFunc = D3D11_COMPARISON_ALWAYS;
    ds.FrontFace = ds.BackFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS};
    if (const auto* s = discard ? nullptr : ci.pDepthStencilState) {
        ds.DepthEnable = s->depthTestEnable;
        ds.DepthWriteMask = s->depthWriteEnable ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
        ds.DepthFunc = (D3D11_COMPARISON_FUNC)(s->depthCompareOp + 1);
        ds.StencilEnable = s->stencilTestEnable;
        ds.StencilReadMask = (UINT8)s->front.compareMask;
        ds.StencilWriteMask = (UINT8)s->front.writeMask;
        auto face = [](const VkStencilOpState& o) {
            return D3D11_DEPTH_STENCILOP_DESC{(D3D11_STENCIL_OP)(o.failOp + 1), (D3D11_STENCIL_OP)(o.depthFailOp + 1),
                                              (D3D11_STENCIL_OP)(o.passOp + 1), (D3D11_COMPARISON_FUNC)(o.compareOp + 1)};
        };
        ds.FrontFace = face(s->front);
        ds.BackFace = face(s->back);
        p->stencilRef = s->front.reference;
    }

    // Blend
    D3D11_BLEND_DESC bd{};
    for (auto& rt : bd.RenderTarget) rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (ci.pMultisampleState && !discard) {
        bd.AlphaToCoverageEnable = ci.pMultisampleState->alphaToCoverageEnable;
        if (ci.pMultisampleState->pSampleMask) p->sampleMask = ci.pMultisampleState->pSampleMask[0];
    }
    if (const auto* b = discard ? nullptr : ci.pColorBlendState) {
        bd.IndependentBlendEnable = b->attachmentCount > 1;
        for (uint32_t i = 0; i < b->attachmentCount && i < 8; i++) {
            const auto& a = b->pAttachments[i];
            auto& rt = bd.RenderTarget[i];
            rt.BlendEnable = a.blendEnable;
            rt.SrcBlend = blendFactor(a.srcColorBlendFactor, false);
            rt.DestBlend = blendFactor(a.dstColorBlendFactor, false);
            rt.BlendOp = (D3D11_BLEND_OP)(a.colorBlendOp + 1);
            rt.SrcBlendAlpha = blendFactor(a.srcAlphaBlendFactor, true);
            rt.DestBlendAlpha = blendFactor(a.dstAlphaBlendFactor, true);
            rt.BlendOpAlpha = (D3D11_BLEND_OP)(a.alphaBlendOp + 1);
            rt.RenderTargetWriteMask = (UINT8)(a.colorWriteMask & 0xf);
            if (!a.blendEnable) {
                rt.SrcBlend = rt.SrcBlendAlpha = D3D11_BLEND_ONE;
                rt.DestBlend = rt.DestBlendAlpha = D3D11_BLEND_ZERO;
                rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
            }
        }
        memcpy(p->blendConstants, b->blendConstants, sizeof(p->blendConstants));
    }
    if (FAILED(d.d3d->CreateBlendState(&bd, p->blend.put()))) VKON_LOG("CreateBlendState failed");

    if (const auto* dyn = ci.pDynamicState)
        for (uint32_t i = 0; i < dyn->dynamicStateCount; i++)
            if (dyn->pDynamicStates[i] < 32) p->dynamicMask |= 1u << dyn->pDynamicStates[i];
    if (const auto* vp = discard ? nullptr : ci.pViewportState) {
        if (vp->pViewports && !(p->dynamicMask & (1u << VK_DYNAMIC_STATE_VIEWPORT)))
            p->viewports.assign(vp->pViewports, vp->pViewports + vp->viewportCount);
        if (vp->pScissors && !(p->dynamicMask & (1u << VK_DYNAMIC_STATE_SCISSOR)))
            p->scissors.assign(vp->pScissors, vp->pScissors + vp->scissorCount);
    }
    *out = p.release();
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateGraphicsPipelines(VkDevice h, VkPipelineCache, uint32_t n,
                                                       const VkGraphicsPipelineCreateInfo* cis,
                                                       const VkAllocationCallbacks*, VkPipeline* out) {
    Device& d = *from<Device>(h);
    VkResult res = VK_SUCCESS;
    for (uint32_t i = 0; i < n; i++) {
        Pipeline* p = nullptr;
        VkResult r = createGraphicsPipeline(d, cis[i], &p);
        out[i] = to<VkPipeline>(p);
        if (r != VK_SUCCESS) res = r;
    }
    return res;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateComputePipelines(VkDevice h, VkPipelineCache, uint32_t n,
                                                      const VkComputePipelineCreateInfo* cis,
                                                      const VkAllocationCallbacks*, VkPipeline* out) {
    Device& d = *from<Device>(h);
    VkResult res = VK_SUCCESS;
    for (uint32_t i = 0; i < n; i++) {
        out[i] = VK_NULL_HANDLE;
        auto p = std::make_unique<Pipeline>();
        p->compute = true;
        p->layout = from<PipelineLayout>(cis[i].layout)->data;
        std::string hlsl, err;
        std::vector<uint8_t> code;
        if (!translateShader(*from<ShaderModule>(cis[i].stage.module), cis[i].stage.pName, VK_SHADER_STAGE_COMPUTE_BIT,
                             *p->layout, cis[i].stage.pSpecializationInfo, hlsl, err)) {
            logShaderError("compute shader translation", err, "");
            res = VK_ERROR_INVALID_SHADER_NV;
            continue;
        }
        if (!compileHLSL(hlsl, "main", "cs_5_0", code, err)) {
            logShaderError("compute shader compile", err, hlsl);
            res = VK_ERROR_INVALID_SHADER_NV;
            continue;
        }
        if (FAILED(d.d3d->CreateComputeShader(code.data(), code.size(), nullptr, p->cs.put()))) {
            res = VK_ERROR_INVALID_SHADER_NV;
            continue;
        }
        out[i] = to<VkPipeline>(p.release());
    }
    return res;
}

VKAPI_ATTR void VKAPI_CALL DestroyPipeline(VkDevice, VkPipeline h, const VkAllocationCallbacks*) { delete from<Pipeline>(h); }

} // namespace vkon11
