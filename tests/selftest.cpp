// VKOn11 self-test: talks to the driver DLL directly (no Vulkan loader needed),
// renders a handful of scenes off-screen and checks the resulting pixels.
//
//   selftest.exe [path\to\vkon11.dll] [--window] [--bench[=draws]] [--allow-skip]
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#define VK_USE_PLATFORM_WIN32_KHR
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "shaders/tri_vert.h"
#include "shaders/tri_frag.h"
#include "shaders/tex_vert.h"
#include "shaders/tex_frag.h"
#include "shaders/inst_vert.h"
#include "shaders/vidx_vert.h"
#include "shaders/instattr_vert.h"
#include "shaders/double_comp.h"
#include "shaders/fill_comp.h"

#define VK_FUNCS(X) \
    X(CreateInstance) X(DestroyInstance) X(EnumeratePhysicalDevices) X(GetPhysicalDeviceProperties) \
    X(GetPhysicalDeviceMemoryProperties) X(GetPhysicalDeviceFormatProperties) X(CreateDevice) X(DestroyDevice) \
    X(GetDeviceQueue) X(QueueSubmit) X(QueueWaitIdle) X(DeviceWaitIdle) X(AllocateMemory) X(FreeMemory) X(MapMemory) \
    X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) X(BindBufferMemory) X(CreateImage) X(DestroyImage) \
    X(GetImageMemoryRequirements) X(BindImageMemory) X(CreateImageView) X(DestroyImageView) X(CreateSampler) \
    X(CreateShaderModule) X(CreateDescriptorSetLayout) X(CreateDescriptorPool) X(AllocateDescriptorSets) \
    X(UpdateDescriptorSets) X(CreatePipelineLayout) X(CreateRenderPass) X(CreateFramebuffer) X(CreateGraphicsPipelines) \
    X(CreateComputePipelines) X(CreateCommandPool) X(AllocateCommandBuffers) X(BeginCommandBuffer) X(EndCommandBuffer) \
    X(CmdBeginRenderPass) X(CmdEndRenderPass) X(CmdBindPipeline) X(CmdSetViewport) X(CmdSetScissor) \
    X(CmdBindDescriptorSets) X(CmdBindIndexBuffer) X(CmdBindVertexBuffers) X(CmdPushConstants) X(CmdDraw) \
    X(CmdDrawIndexed) X(CmdDrawIndexedIndirect) X(CmdDispatch) X(CmdCopyBuffer) X(CmdUpdateBuffer) X(CmdFillBuffer) \
    X(CmdCopyBufferToImage) X(CmdCopyImageToBuffer) X(CmdCopyImage) X(CmdBlitImage) X(CmdClearColorImage) \
    X(CmdClearAttachments) X(CmdPipelineBarrier) X(CreateFence) X(WaitForFences) X(ResetFences) \
    X(CreateWin32SurfaceKHR) X(GetPhysicalDeviceSurfaceCapabilitiesKHR) X(CreateSwapchainKHR) X(GetSwapchainImagesKHR) \
    X(AcquireNextImageKHR) X(QueuePresentKHR) X(DestroySwapchainKHR) X(DestroySurfaceKHR)

#define X(n) static PFN_vk##n vk##n;
VK_FUNCS(X)
#undef X

static int g_failures = 0, g_checks = 0, g_skips = 0;
// Some shaders need HLSL features that Wine's built-in compiler lacks. With
// --allow-skip, a pipeline marked optional that fails to compile is skipped
// instead of failed. On real Windows nothing should need skipping.
static bool g_allowSkip = false, g_optional = false;
static bool created(VkResult r, const char* what) {
    if (r == VK_SUCCESS) return true;
    if (g_optional && g_allowSkip) { printf("    SKIPPED: %s failed (%d) with this shader compiler\n", what, (int)r); g_skips++; }
    else { printf("    FAIL: %s -> %d\n", what, (int)r); g_failures++; }
    return false;
}
static void check(bool ok, const char* fmt, ...) {
    g_checks++;
    if (ok) return;
    g_failures++;
    va_list ap;
    va_start(ap, fmt);
    printf("    FAIL: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}
#define VK_OK(expr) do { VkResult r_ = (expr); if (r_ != VK_SUCCESS) { printf("    FAIL: %s -> %d\n", #expr, (int)r_); g_failures++; } } while (0)

static VkInstance g_inst;
static VkPhysicalDevice g_pd;
static VkDevice g_dev;
static VkQueue g_queue;
static VkCommandPool g_pool;
static VkCommandBuffer g_cmd;

struct Buf { VkBuffer b = VK_NULL_HANDLE; VkDeviceMemory m = VK_NULL_HANDLE; uint8_t* map = nullptr; };
static Buf makeBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host = true) {
    Buf r;
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = size;
    ci.usage = usage;
    VK_OK(vkCreateBuffer(g_dev, &ci, nullptr, &r.b));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g_dev, r.b, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = host ? 1 : 0;
    VK_OK(vkAllocateMemory(g_dev, &ai, nullptr, &r.m));
    VK_OK(vkBindBufferMemory(g_dev, r.b, r.m, 0));
    if (host) VK_OK(vkMapMemory(g_dev, r.m, 0, VK_WHOLE_SIZE, 0, (void**)&r.map));
    return r;
}

struct Img { VkImage i = VK_NULL_HANDLE; VkDeviceMemory m = VK_NULL_HANDLE; VkImageView v = VK_NULL_HANDLE; uint32_t w = 0, h = 0; };
static Img makeImage(VkFormat fmt, uint32_t w, uint32_t h, VkImageUsageFlags usage, VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT,
                     VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT, uint32_t mips = 1) {
    Img r;
    r.w = w;
    r.h = h;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent = {w, h, 1};
    ci.mipLevels = mips;
    ci.arrayLayers = 1;
    ci.samples = samples;
    ci.usage = usage;
    VK_OK(vkCreateImage(g_dev, &ci, nullptr, &r.i));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(g_dev, r.i, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = 0;
    VK_OK(vkAllocateMemory(g_dev, &ai, nullptr, &r.m));
    VK_OK(vkBindImageMemory(g_dev, r.i, r.m, 0));
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = r.i;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange = {aspect, 0, mips, 0, 1};
    VK_OK(vkCreateImageView(g_dev, &vi, nullptr, &r.v));
    return r;
}

static void begin() {
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VK_OK(vkBeginCommandBuffer(g_cmd, &bi));
}
static void submit() {
    VK_OK(vkEndCommandBuffer(g_cmd));
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &g_cmd;
    VK_OK(vkQueueSubmit(g_queue, 1, &si, VK_NULL_HANDLE));
    VK_OK(vkQueueWaitIdle(g_queue));
}

static std::vector<uint8_t> readback(const Img& img, uint32_t mip = 0) {
    uint32_t w = std::max(1u, img.w >> mip), h = std::max(1u, img.h >> mip);
    Buf b = makeBuffer(w * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    begin();
    VkBufferImageCopy c{};
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1};
    c.imageExtent = {w, h, 1};
    vkCmdCopyImageToBuffer(g_cmd, img.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b.b, 1, &c);
    submit();
    std::vector<uint8_t> px(b.map, b.map + w * h * 4);
    vkDestroyBuffer(g_dev, b.b, nullptr);
    vkFreeMemory(g_dev, b.m, nullptr);
    return px;
}

static void expectPixel(const char* what, const std::vector<uint8_t>& px, uint32_t w, uint32_t x, uint32_t y, int r, int g, int b,
                        int tol = 3) {
    const uint8_t* p = &px[(y * w + x) * 4];
    bool ok = std::abs(p[0] - r) <= tol && std::abs(p[1] - g) <= tol && std::abs(p[2] - b) <= tol;
    check(ok, "%s: pixel (%u,%u) is (%d,%d,%d), expected (%d,%d,%d)", what, x, y, p[0], p[1], p[2], r, g, b);
}

static void writeBmp(const char* path, const std::vector<uint8_t>& px, uint32_t w, uint32_t h) {
    FILE* f = fopen(path, "wb");
    if (!f) return;
    uint32_t row = w * 3 + ((4 - (w * 3) % 4) % 4), size = 54 + row * h;
    uint8_t hdr[54] = {'B', 'M'};
    memcpy(hdr + 2, &size, 4);
    uint32_t off = 54, ih = 40, bpp = 24 << 16 | 1;
    memcpy(hdr + 10, &off, 4);
    memcpy(hdr + 14, &ih, 4);
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    memcpy(hdr + 26, &bpp, 4);
    fwrite(hdr, 1, 54, f);
    std::vector<uint8_t> line(row, 0);
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t* s = &px[(h - 1 - y) * w * 4];
        for (uint32_t x = 0; x < w; x++) { line[x * 3] = s[x * 4 + 2]; line[x * 3 + 1] = s[x * 4 + 1]; line[x * 3 + 2] = s[x * 4]; }
        fwrite(line.data(), 1, row, f);
    }
    fclose(f);
}

template <size_t N> static VkShaderModule shader(const uint32_t (&code)[N]) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = N * 4;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    VK_OK(vkCreateShaderModule(g_dev, &ci, nullptr, &m));
    return m;
}

static VkRenderPass makeRenderPass(VkFormat color, VkFormat depth, VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT, bool resolve = false) {
    std::vector<VkAttachmentDescription> a;
    VkAttachmentDescription c{};
    c.format = color;
    c.samples = samples;
    c.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    c.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    c.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    a.push_back(c);
    VkAttachmentReference cref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}, dref{VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_UNDEFINED},
        rref{VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_UNDEFINED};
    if (depth != VK_FORMAT_UNDEFINED) {
        VkAttachmentDescription d = c;
        d.format = depth;
        d.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        d.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        dref = {(uint32_t)a.size(), VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        a.push_back(d);
    }
    if (resolve) {
        VkAttachmentDescription r = c;
        r.samples = VK_SAMPLE_COUNT_1_BIT;
        r.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        rref = {(uint32_t)a.size(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        a.push_back(r);
    }
    VkSubpassDescription sp{};
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = 1;
    sp.pColorAttachments = &cref;
    if (depth != VK_FORMAT_UNDEFINED) sp.pDepthStencilAttachment = &dref;
    if (resolve) sp.pResolveAttachments = &rref;
    VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    ci.attachmentCount = (uint32_t)a.size();
    ci.pAttachments = a.data();
    ci.subpassCount = 1;
    ci.pSubpasses = &sp;
    VkRenderPass rp = VK_NULL_HANDLE;
    VK_OK(vkCreateRenderPass(g_dev, &ci, nullptr, &rp));
    return rp;
}

static VkFramebuffer makeFramebuffer(VkRenderPass rp, std::vector<VkImageView> views, uint32_t w, uint32_t h) {
    VkFramebufferCreateInfo ci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    ci.renderPass = rp;
    ci.attachmentCount = (uint32_t)views.size();
    ci.pAttachments = views.data();
    ci.width = w;
    ci.height = h;
    ci.layers = 1;
    VkFramebuffer fb = VK_NULL_HANDLE;
    VK_OK(vkCreateFramebuffer(g_dev, &ci, nullptr, &fb));
    return fb;
}

struct PipelineDesc {
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkRenderPass rp = VK_NULL_HANDLE;
    std::vector<VkVertexInputBindingDescription> bindings;
    std::vector<VkVertexInputAttributeDescription> attrs;
    VkCullModeFlags cull = VK_CULL_MODE_NONE;
    VkFrontFace front = VK_FRONT_FACE_CLOCKWISE;
    bool depth = false;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
};
static VkPipeline makePipeline(const PipelineDesc& d) {
    VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    st[0].module = d.vs;
    st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    st[1].module = d.fs;
    st[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = (uint32_t)d.bindings.size();
    vi.pVertexBindingDescriptions = d.bindings.data();
    vi.vertexAttributeDescriptionCount = (uint32_t)d.attrs.size();
    vi.pVertexAttributeDescriptions = d.attrs.data();
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = d.cull;
    rs.frontFace = d.front;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = d.samples;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = d.depth;
    ds.depthWriteEnable = d.depth;
    ds.depthCompareOp = VK_COMPARE_OP_LESS;
    VkPipelineColorBlendAttachmentState ba{};
    ba.colorWriteMask = 0xf;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &ba;
    VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynStates;
    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.stageCount = 2;
    ci.pStages = st;
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &ds;
    ci.pColorBlendState = &cb;
    ci.pDynamicState = &dyn;
    ci.layout = d.layout;
    ci.renderPass = d.rp;
    VkPipeline p = VK_NULL_HANDLE;
    if (!created(vkCreateGraphicsPipelines(g_dev, VK_NULL_HANDLE, 1, &ci, nullptr, &p), "vkCreateGraphicsPipelines")) return VK_NULL_HANDLE;
    return p;
}

static void beginPass(VkRenderPass rp, VkFramebuffer fb, uint32_t w, uint32_t h, float r, float g, float b) {
    VkClearValue cv[3]{};
    cv[0].color = {{r, g, b, 1.0f}};
    cv[1].depthStencil = {1.0f, 0};
    cv[2].color = {{r, g, b, 1.0f}};
    VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    bi.renderPass = rp;
    bi.framebuffer = fb;
    bi.renderArea = {{0, 0}, {w, h}};
    bi.clearValueCount = 3;
    bi.pClearValues = cv;
    vkCmdBeginRenderPass(g_cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp{0, 0, (float)w, (float)h, 0, 1};
    VkRect2D sc{{0, 0}, {w, h}};
    vkCmdSetViewport(g_cmd, 0, 1, &vp);
    vkCmdSetScissor(g_cmd, 0, 1, &sc);
}

static VkDescriptorPool g_dpool;
static VkDescriptorSet allocSet(VkDescriptorSetLayout l) {
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = g_dpool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &l;
    VkDescriptorSet s = VK_NULL_HANDLE;
    VK_OK(vkAllocateDescriptorSets(g_dev, &ai, &s));
    return s;
}
static VkDescriptorSetLayout setLayout(std::vector<VkDescriptorSetLayoutBinding> b) {
    VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ci.bindingCount = (uint32_t)b.size();
    ci.pBindings = b.data();
    VkDescriptorSetLayout l = VK_NULL_HANDLE;
    VK_OK(vkCreateDescriptorSetLayout(g_dev, &ci, nullptr, &l));
    return l;
}
static VkPipelineLayout pipeLayout(VkDescriptorSetLayout* l, VkShaderStageFlags pushStages, uint32_t pushSize) {
    VkPushConstantRange pr{pushStages, 0, pushSize};
    VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    ci.setLayoutCount = l ? 1 : 0;
    ci.pSetLayouts = l;
    ci.pushConstantRangeCount = pushSize ? 1 : 0;
    ci.pPushConstantRanges = &pr;
    VkPipelineLayout pl = VK_NULL_HANDLE;
    VK_OK(vkCreatePipelineLayout(g_dev, &ci, nullptr, &pl));
    return pl;
}

static const VkFormat kColor = VK_FORMAT_R8G8B8A8_UNORM;
static const VkImageUsageFlags kTargetUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                              VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

// ---------------------------------------------------------------------------
static void testClear() {
    printf("[clear] render pass clear + readback\n");
    Img rt = makeImage(kColor, 64, 64, kTargetUsage);
    VkRenderPass rp = makeRenderPass(kColor, VK_FORMAT_UNDEFINED);
    VkFramebuffer fb = makeFramebuffer(rp, {rt.v}, 64, 64);
    begin();
    beginPass(rp, fb, 64, 64, 0.25f, 0.5f, 0.75f);
    VkClearAttachment ca{VK_IMAGE_ASPECT_COLOR_BIT, 0, {}};
    ca.clearValue.color = {{1.0f, 0.0f, 0.0f, 1.0f}};
    VkClearRect cr{{{8, 8}, {16, 16}}, 0, 1};
    vkCmdClearAttachments(g_cmd, 1, &ca, 1, &cr);
    vkCmdEndRenderPass(g_cmd);
    submit();
    auto px = readback(rt);
    expectPixel("clear colour", px, 64, 40, 40, 64, 128, 191);
    expectPixel("clear rect", px, 64, 12, 12, 255, 0, 0);
    expectPixel("outside clear rect", px, 64, 30, 12, 64, 128, 191);
}

static Img g_scene; // result of the textured test, reused by the blit test

static void testTriangle() {
    printf("[triangle] vertex buffer + uniform buffer + push constants + culling\n");
    Img rt = makeImage(kColor, 64, 64, kTargetUsage);
    VkRenderPass rp = makeRenderPass(kColor, VK_FORMAT_UNDEFINED);
    VkFramebuffer fb = makeFramebuffer(rp, {rt.v}, 64, 64);

    struct V { float x, y, r, g, b; };
    // top-left, top-right, bottom-left in Vulkan clip space (Y points down): clockwise on screen.
    const V verts[] = {{-1, -1, 1, 1, 1}, {1, -1, 1, 1, 1}, {-1, 1, 1, 1, 1}};
    Buf vb = makeBuffer(sizeof(verts), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    memcpy(vb.map, verts, sizeof(verts));
    struct { float tint[4]; float offset[2]; float pad[2]; } ubo = {{1.0f, 0.5f, 0.25f, 1.0f}, {0, 0}, {0, 0}};
    Buf ub = makeBuffer(256, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    memcpy(ub.map + 64, &ubo, sizeof(ubo)); // deliberately at a non-zero offset

    VkDescriptorSetLayout sl = setLayout({{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr}});
    VkPipelineLayout pl = pipeLayout(&sl, VK_SHADER_STAGE_VERTEX_BIT, 16);
    VkDescriptorSet set = allocSet(sl);
    VkDescriptorBufferInfo bi{ub.b, 64, sizeof(ubo)};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    w.pBufferInfo = &bi;
    vkUpdateDescriptorSets(g_dev, 1, &w, 0, nullptr);

    PipelineDesc pd;
    pd.vs = shader(spv_tri_vert);
    pd.fs = shader(spv_tri_frag);
    pd.layout = pl;
    pd.rp = rp;
    pd.bindings = {{0, sizeof(V), VK_VERTEX_INPUT_RATE_VERTEX}};
    pd.attrs = {{0, 0, VK_FORMAT_R32G32_SFLOAT, 0}, {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 8}};
    pd.cull = VK_CULL_MODE_BACK_BIT;
    pd.front = VK_FRONT_FACE_CLOCKWISE;
    VkPipeline visible = makePipeline(pd);
    pd.front = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    VkPipeline culled = makePipeline(pd);
    if (!visible || !culled) return;

    for (int pass = 0; pass < 2; pass++) {
        begin();
        beginPass(rp, fb, 64, 64, 0, 0, 0);
        vkCmdBindPipeline(g_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pass == 0 ? visible : culled);
        vkCmdBindDescriptorSets(g_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &set, 0, nullptr);
        VkDeviceSize zero = 0;
        vkCmdBindVertexBuffers(g_cmd, 0, 1, &vb.b, &zero);
        float scale[4] = {1, 1, 0, 0};
        vkCmdPushConstants(g_cmd, pl, VK_SHADER_STAGE_VERTEX_BIT, 0, 16, scale);
        vkCmdDraw(g_cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(g_cmd);
        submit();
        auto px = readback(rt);
        if (pass == 0) {
            writeBmp("selftest_triangle.bmp", px, 64, 64);
            expectPixel("inside triangle (top-left half)", px, 64, 40, 8, 255, 128, 64);
            expectPixel("outside triangle (bottom-right half)", px, 64, 40, 56, 0, 0, 0);
        } else {
            expectPixel("back-face culled", px, 64, 40, 8, 0, 0, 0);
        }
    }

    // VK_KHR_maintenance1: a negative viewport height mirrors the image vertically,
    // which also reverses the on-screen winding (so the "culled" pipeline now draws).
    {
        begin();
        beginPass(rp, fb, 64, 64, 0, 0, 0);
        VkViewport flipped{0, 64, 64, -64, 0, 1};
        vkCmdSetViewport(g_cmd, 0, 1, &flipped);
        vkCmdBindPipeline(g_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, culled);
        vkCmdBindDescriptorSets(g_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &set, 0, nullptr);
        VkDeviceSize zero = 0;
        vkCmdBindVertexBuffers(g_cmd, 0, 1, &vb.b, &zero);
        float scale[4] = {1, 1, 0, 0};
        vkCmdPushConstants(g_cmd, pl, VK_SHADER_STAGE_VERTEX_BIT, 0, 16, scale);
        vkCmdDraw(g_cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(g_cmd);
        submit();
        auto px = readback(rt);
        expectPixel("flipped viewport: inside (bottom-left half)", px, 64, 8, 56, 255, 128, 64);
        expectPixel("flipped viewport: outside (top-right half)", px, 64, 40, 8, 0, 0, 0);
    }

    // Host writes after the first use must reach the GPU (dirty page tracking).
    ((V*)vb.map)[1].x = -1.0f; // collapse the triangle to zero area
    ubo.tint[1] = 1.0f;
    memcpy(ub.map + 64, &ubo, sizeof(ubo));
    begin();
    beginPass(rp, fb, 64, 64, 0, 0, 0);
    vkCmdBindPipeline(g_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, visible);
    vkCmdBindDescriptorSets(g_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &set, 0, nullptr);
    VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(g_cmd, 0, 1, &vb.b, &zero);
    float scale[4] = {1, 1, 0, 0};
    vkCmdPushConstants(g_cmd, pl, VK_SHADER_STAGE_VERTEX_BIT, 0, 16, scale);
    vkCmdDraw(g_cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(g_cmd);
    submit();
    expectPixel("vertex buffer re-upload after host write", readback(rt), 64, 40, 8, 0, 0, 0);
}

static void testTextured() {
    printf("[textured] index buffer + texture upload + sampler + depth test + scissor\n");
    Img rt = makeImage(kColor, 64, 64, kTargetUsage);
    Img depth = makeImage(VK_FORMAT_D32_SFLOAT, 64, 64, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
    VkRenderPass rp = makeRenderPass(kColor, VK_FORMAT_D32_SFLOAT);
    VkFramebuffer fb = makeFramebuffer(rp, {rt.v, depth.v}, 64, 64);

    Img tex = makeImage(kColor, 2, 2, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    const uint8_t texels[16] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
    Buf staging = makeBuffer(16, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    memcpy(staging.map, texels, 16);
    begin();
    VkBufferImageCopy c{};
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageExtent = {2, 2, 1};
    vkCmdCopyBufferToImage(g_cmd, staging.b, tex.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    submit();

    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkSampler smp = VK_NULL_HANDLE;
    VK_OK(vkCreateSampler(g_dev, &sci, nullptr, &smp));

    struct V { float x, y, z, u, v; };
    const V verts[] = {{-1, -1, 0, 0, 0}, {1, -1, 0, 1, 0}, {1, 1, 0, 1, 1}, {-1, 1, 0, 0, 1}};
    const uint16_t idx[] = {0, 1, 2, 0, 2, 3};
    // Vertices live in device-local memory and are filled through a staging copy.
    Buf vb = makeBuffer(sizeof(verts), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
    Buf vstage = makeBuffer(sizeof(verts), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    memcpy(vstage.map, verts, sizeof(verts));
    Buf ib = makeBuffer(sizeof(idx), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    memcpy(ib.map, idx, sizeof(idx));
    Buf indirect = makeBuffer(sizeof(VkDrawIndexedIndirectCommand), VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
    VkDrawIndexedIndirectCommand ic{6, 1, 0, 0, 0};
    memcpy(indirect.map, &ic, sizeof(ic));

    VkDescriptorSetLayout sl = setLayout({{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}});
    VkPipelineLayout pl = pipeLayout(&sl, VK_SHADER_STAGE_VERTEX_BIT, 20);
    VkDescriptorSet set = allocSet(sl);
    VkDescriptorImageInfo ii{smp, tex.v, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(g_dev, 1, &w, 0, nullptr);

    PipelineDesc pd;
    pd.vs = shader(spv_tex_vert);
    pd.fs = shader(spv_tex_frag);
    pd.layout = pl;
    pd.rp = rp;
    pd.bindings = {{0, sizeof(V), VK_VERTEX_INPUT_RATE_VERTEX}};
    pd.attrs = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0}, {1, 0, VK_FORMAT_R32G32_SFLOAT, 12}};
    pd.depth = true;
    VkPipeline pipe = makePipeline(pd);
    if (!pipe) return;

    begin();
    VkBufferCopy bc{0, 0, sizeof(verts)};
    vkCmdCopyBuffer(g_cmd, vstage.b, vb.b, 1, &bc);
    beginPass(rp, fb, 64, 64, 0.1f, 0.1f, 0.1f);
    vkCmdBindPipeline(g_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindDescriptorSets(g_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &set, 0, nullptr);
    VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(g_cmd, 0, 1, &vb.b, &zero);
    vkCmdBindIndexBuffer(g_cmd, ib.b, 0, VK_INDEX_TYPE_UINT16);
    struct { float mul[4]; float depth; } pc = {{1, 1, 1, 1}, 0.5f};
    vkCmdPushConstants(g_cmd, pl, VK_SHADER_STAGE_VERTEX_BIT, 0, 20, &pc);
    vkCmdDrawIndexed(g_cmd, 6, 1, 0, 0, 0);
    pc = {{0, 0, 0, 1}, 0.75f}; // behind: must be rejected by the depth test
    vkCmdPushConstants(g_cmd, pl, VK_SHADER_STAGE_VERTEX_BIT, 0, 20, &pc);
    vkCmdDrawIndexedIndirect(g_cmd, indirect.b, 0, 1, sizeof(ic));
    pc = {{0.5f, 0.5f, 0.5f, 1}, 0.25f}; // in front, but scissored to the right half
    vkCmdPushConstants(g_cmd, pl, VK_SHADER_STAGE_VERTEX_BIT, 0, 20, &pc);
    VkRect2D sc{{32, 0}, {32, 64}};
    vkCmdSetScissor(g_cmd, 0, 1, &sc);
    vkCmdDrawIndexed(g_cmd, 6, 1, 0, 0, 0);
    vkCmdEndRenderPass(g_cmd);
    submit();
    auto px = readback(rt);
    writeBmp("selftest_textured.bmp", px, 64, 64);
    expectPixel("texel top-left (red)", px, 64, 16, 16, 255, 0, 0);
    expectPixel("texel top-right (green, dimmed by scissored draw)", px, 64, 48, 16, 0, 128, 0);
    expectPixel("texel bottom-left (blue)", px, 64, 16, 48, 0, 0, 255);
    expectPixel("texel bottom-right (white, dimmed)", px, 64, 48, 48, 128, 128, 128);
    g_scene = rt;
}

// Draws one unit quad per call with the vidx shader (firstVertex = 10).
struct QuadPC { float color[4]; float origin[2]; };
static void drawQuad(VkPipelineLayout pl, float ox, float oy, float r, float g, float b) {
    QuadPC pc = {{r, g, b, 1}, {ox, oy}};
    vkCmdPushConstants(g_cmd, pl, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
    vkCmdDraw(g_cmd, 6, 1, 10, 0);
}

static void testVertexIndex() {
    printf("[vertex-index] gl_VertexIndex with non-zero firstVertex, no vertex buffers\n");
    Img rt = makeImage(kColor, 64, 64, kTargetUsage);
    VkRenderPass rp = makeRenderPass(kColor, VK_FORMAT_UNDEFINED);
    VkFramebuffer fb = makeFramebuffer(rp, {rt.v}, 64, 64);
    PipelineDesc pd;
    pd.vs = shader(spv_vidx_vert);
    pd.fs = shader(spv_tri_frag);
    pd.layout = pipeLayout(nullptr, VK_SHADER_STAGE_VERTEX_BIT, sizeof(QuadPC));
    pd.rp = rp;
    VkPipeline pipe = makePipeline(pd);
    if (!pipe) return;
    begin();
    beginPass(rp, fb, 64, 64, 0.5f, 0.5f, 0.5f);
    vkCmdBindPipeline(g_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    drawQuad(pd.layout, 0, 0, 1, 0, 0);
    drawQuad(pd.layout, 1, 1, 0, 0, 1);
    vkCmdEndRenderPass(g_cmd);
    submit();
    auto px = readback(rt);
    expectPixel("quad at top-left", px, 64, 16, 16, 255, 0, 0);
    expectPixel("quad at bottom-right", px, 64, 48, 48, 0, 0, 255);
    expectPixel("untouched top-right", px, 64, 48, 16, 128, 128, 128);
}

static void testInstancing() {
    printf("[instancing] per-instance vertex attributes with non-zero firstInstance\n");
    Img rt = makeImage(kColor, 64, 64, kTargetUsage);
    VkRenderPass rp = makeRenderPass(kColor, VK_FORMAT_UNDEFINED);
    VkFramebuffer fb = makeFramebuffer(rp, {rt.v}, 64, 64);
    struct I { float ox, oy, r, g, b; };
    // Entry 0 must be skipped by firstInstance = 1.
    const I inst[] = {{0, 0, 1, 1, 1}, {0, 0, 0, 0, 1}, {1, 0, 1, 0, 0}, {0, 1, 0, 1, 0}, {1, 1, 1, 1, 0}};
    Buf vb = makeBuffer(sizeof(inst), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    memcpy(vb.map, inst, sizeof(inst));
    PipelineDesc pd;
    pd.vs = shader(spv_instattr_vert);
    pd.fs = shader(spv_tri_frag);
    pd.layout = pipeLayout(nullptr, 0, 0);
    pd.rp = rp;
    pd.bindings = {{0, sizeof(I), VK_VERTEX_INPUT_RATE_INSTANCE}};
    pd.attrs = {{0, 0, VK_FORMAT_R32G32_SFLOAT, 0}, {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 8}};
    VkPipeline pipe = makePipeline(pd);
    if (pipe) {
        begin();
        beginPass(rp, fb, 64, 64, 0.5f, 0.5f, 0.5f);
        vkCmdBindPipeline(g_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        VkDeviceSize zero = 0;
        vkCmdBindVertexBuffers(g_cmd, 0, 1, &vb.b, &zero);
        vkCmdDraw(g_cmd, 6, 4, 0, 1);
        vkCmdEndRenderPass(g_cmd);
        submit();
        auto px = readback(rt);
        writeBmp("selftest_instancing.bmp", px, 64, 64);
        expectPixel("instance 0", px, 64, 16, 16, 0, 0, 255);
        expectPixel("instance 1", px, 64, 48, 16, 255, 0, 0);
        expectPixel("instance 2", px, 64, 16, 48, 0, 255, 0);
        expectPixel("instance 3", px, 64, 48, 48, 255, 255, 0);
    }

    printf("[instance-index] gl_InstanceIndex with non-zero firstInstance\n");
    PipelineDesc pi;
    pi.vs = shader(spv_inst_vert);
    pi.fs = shader(spv_tri_frag);
    pi.layout = pipeLayout(nullptr, 0, 0);
    pi.rp = rp;
    g_optional = true; // needs SV_InstanceID, which Wine's HLSL compiler lacks
    pipe = makePipeline(pi);
    g_optional = false;
    if (!pipe) return;
    begin();
    beginPass(rp, fb, 64, 64, 0.5f, 0.5f, 0.5f);
    vkCmdBindPipeline(g_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdDraw(g_cmd, 6, 4, 10, 5);
    vkCmdEndRenderPass(g_cmd);
    submit();
    auto px = readback(rt);
    expectPixel("instance 0", px, 64, 16, 16, 0, 0, 255);
    expectPixel("instance 1", px, 64, 48, 16, 255, 0, 170);
    expectPixel("instance 2", px, 64, 16, 48, 0, 255, 85);
    expectPixel("instance 3", px, 64, 48, 48, 255, 255, 0);
}

static void testTransfers() {
    printf("[transfers] buffer copy/fill/update, image copy, blit, clear image\n");
    Buf a = makeBuffer(64, VK_BUFFER_USAGE_TRANSFER_SRC_BIT), b = makeBuffer(64, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    for (int i = 0; i < 64; i++) a.map[i] = (uint8_t)i;
    memset(b.map, 0xee, 64);
    Img copy = makeImage(kColor, 64, 64, kTargetUsage);
    Img small = makeImage(kColor, 32, 32, kTargetUsage);
    Img cleared = makeImage(kColor, 16, 16, kTargetUsage);
    begin();
    VkBufferCopy bc{8, 0, 16};
    vkCmdCopyBuffer(g_cmd, a.b, b.b, 1, &bc);
    vkCmdFillBuffer(g_cmd, b.b, 16, 8, 0x11223344);
    const uint32_t upd[2] = {0xdeadbeef, 0x01020304};
    vkCmdUpdateBuffer(g_cmd, b.b, 32, 8, upd);
    if (g_scene.i) {
        VkImageCopy ic{};
        ic.srcSubresource = ic.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        ic.extent = {64, 64, 1};
        vkCmdCopyImage(g_cmd, g_scene.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, copy.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
        VkImageBlit bl{};
        bl.srcSubresource = bl.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        bl.srcOffsets[1] = {64, 64, 1};
        bl.dstOffsets[1] = {32, 32, 1};
        vkCmdBlitImage(g_cmd, g_scene.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, small.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
    }
    VkClearColorValue cc{{0.0f, 1.0f, 0.0f, 1.0f}};
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(g_cmd, cleared.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cc, 1, &range);
    submit();
    check(b.map[0] == 8 && b.map[15] == 23, "vkCmdCopyBuffer: got %d..%d, expected 8..23", b.map[0], b.map[15]);
    check(!memcmp(b.map + 16, "\x44\x33\x22\x11\x44\x33\x22\x11", 8), "vkCmdFillBuffer pattern mismatch");
    check(!memcmp(b.map + 32, upd, 8), "vkCmdUpdateBuffer data mismatch");
    check(b.map[48] == 0xee, "bytes outside the written ranges were modified");
    if (g_scene.i) {
        auto px = readback(copy);
        expectPixel("vkCmdCopyImage", px, 64, 16, 16, 255, 0, 0);
        px = readback(small);
        expectPixel("vkCmdBlitImage top-left", px, 32, 8, 8, 255, 0, 0);
        expectPixel("vkCmdBlitImage bottom-left", px, 32, 8, 24, 0, 0, 255);
        expectPixel("vkCmdBlitImage top-right", px, 32, 24, 8, 0, 128, 0);
    }
    expectPixel("vkCmdClearColorImage", readback(cleared), 16, 8, 8, 0, 255, 0);
}

static void testMsaa() {
    printf("[msaa] 4x multisampled render target with resolve\n");
    Img ms = makeImage(kColor, 64, 64, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_IMAGE_ASPECT_COLOR_BIT, VK_SAMPLE_COUNT_4_BIT);
    Img rt = makeImage(kColor, 64, 64, kTargetUsage);
    if (!ms.i) return;
    VkRenderPass rp = makeRenderPass(kColor, VK_FORMAT_UNDEFINED, VK_SAMPLE_COUNT_4_BIT, true);
    VkFramebuffer fb = makeFramebuffer(rp, {ms.v, rt.v}, 64, 64);
    PipelineDesc pd;
    pd.vs = shader(spv_vidx_vert);
    pd.fs = shader(spv_tri_frag);
    pd.layout = pipeLayout(nullptr, VK_SHADER_STAGE_VERTEX_BIT, sizeof(QuadPC));
    pd.rp = rp;
    pd.samples = VK_SAMPLE_COUNT_4_BIT;
    VkPipeline pipe = makePipeline(pd);
    if (!pipe) return;
    begin();
    beginPass(rp, fb, 64, 64, 0.5f, 0.5f, 0.5f);
    vkCmdBindPipeline(g_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    drawQuad(pd.layout, 0, 0, 0, 0, 1);
    drawQuad(pd.layout, 1, 1, 1, 1, 0);
    vkCmdEndRenderPass(g_cmd);
    submit();
    auto px = readback(rt);
    expectPixel("resolved quad 0", px, 64, 16, 16, 0, 0, 255);
    expectPixel("resolved quad 1", px, 64, 48, 48, 255, 255, 0);
    expectPixel("resolved background", px, 64, 48, 16, 128, 128, 128);
}

static void testCompute() {
    printf("[compute] storage buffer written by a compute shader\n");
    Buf data = makeBuffer(64 * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    for (uint32_t i = 0; i < 64; i++) ((uint32_t*)data.map)[i] = i;
    Buf params = makeBuffer(16, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    *(uint32_t*)params.map = 7;
    VkDescriptorSetLayout sl = setLayout({{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                                          {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}});
    VkPipelineLayout pl = pipeLayout(&sl, 0, 0);
    VkDescriptorSet set = allocSet(sl);
    VkDescriptorBufferInfo bi[2] = {{data.b, 0, VK_WHOLE_SIZE}, {params.b, 0, 16}};
    VkWriteDescriptorSet w[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
    w[0].dstSet = w[1].dstSet = set;
    w[0].descriptorCount = w[1].descriptorCount = 1;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w[0].pBufferInfo = &bi[0];
    w[1].dstBinding = 1;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    w[1].pBufferInfo = &bi[1];
    vkUpdateDescriptorSets(g_dev, 2, w, 0, nullptr);
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = shader(spv_double_comp);
    ci.stage.pName = "main";
    ci.layout = pl;
    VkPipeline pipe = VK_NULL_HANDLE;
    g_optional = true; // needs RWByteAddressBuffer, which Wine's HLSL compiler lacks
    bool ok = created(vkCreateComputePipelines(g_dev, VK_NULL_HANDLE, 1, &ci, nullptr, &pipe), "vkCreateComputePipelines");
    g_optional = false;
    if (!ok || !pipe) return;
    begin();
    vkCmdBindPipeline(g_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(g_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &set, 0, nullptr);
    vkCmdDispatch(g_cmd, 4, 1, 1);
    submit();
    for (uint32_t i = 0; i < 64; i++) ok &= ((uint32_t*)data.map)[i] == i * 2 + 7;
    check(ok, "compute result mismatch: values[1]=%u (expected 9), values[63]=%u (expected 133)", ((uint32_t*)data.map)[1],
          ((uint32_t*)data.map)[63]);
}

static void testComputeImage() {
    printf("[compute-image] storage image written by a compute shader\n");
    Img img = makeImage(kColor, 64, 64, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    if (!img.i) return;
    Buf params = makeBuffer(16, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    const float color[4] = {0, 0, 0.5f, 1};
    memcpy(params.map, color, sizeof(color));
    VkDescriptorSetLayout sl = setLayout({{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                                          {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}});
    VkPipelineLayout pl = pipeLayout(&sl, 0, 0);
    VkDescriptorSet set = allocSet(sl);
    VkDescriptorImageInfo ii{VK_NULL_HANDLE, img.v, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorBufferInfo bi{params.b, 0, 16};
    VkWriteDescriptorSet w[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
    w[0].dstSet = w[1].dstSet = set;
    w[0].descriptorCount = w[1].descriptorCount = 1;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w[0].pImageInfo = &ii;
    w[1].dstBinding = 1;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    w[1].pBufferInfo = &bi;
    vkUpdateDescriptorSets(g_dev, 2, w, 0, nullptr);
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = shader(spv_fill_comp);
    ci.stage.pName = "main";
    ci.layout = pl;
    VkPipeline pipe = VK_NULL_HANDLE;
    g_optional = true; // needs RWTexture2D support in the HLSL compiler
    bool ok = created(vkCreateComputePipelines(g_dev, VK_NULL_HANDLE, 1, &ci, nullptr, &pipe), "vkCreateComputePipelines");
    g_optional = false;
    if (!ok || !pipe) return;
    begin();
    vkCmdBindPipeline(g_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(g_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &set, 0, nullptr);
    vkCmdDispatch(g_cmd, 8, 8, 1);
    submit();
    auto px = readback(img);
    expectPixel("compute image top-left", px, 64, 0, 0, 0, 0, 128);
    expectPixel("compute image bottom-right", px, 64, 63, 63, 255, 255, 128);
    expectPixel("compute image top-right", px, 64, 63, 0, 255, 0, 128);
}

// ---------------------------------------------------------------------------
static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

static void testWindow(int frames) {
    printf("[window] swapchain + present (%d frames)\n", frames);
    WNDCLASSA wc{};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "vkon11_selftest";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA(wc.lpszClassName, "VKOn11 self-test", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 640, 480, nullptr,
                              nullptr, wc.hInstance, nullptr);
    if (!hwnd) { check(false, "CreateWindow failed"); return; }
    VkWin32SurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    sci.hinstance = wc.hInstance;
    sci.hwnd = hwnd;
    VkSurfaceKHR surf = VK_NULL_HANDLE;
    VK_OK(vkCreateWin32SurfaceKHR(g_inst, &sci, nullptr, &surf));
    VkSurfaceCapabilitiesKHR caps{};
    VK_OK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_pd, surf, &caps));
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = surf;
    ci.minImageCount = 2;
    ci.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
    ci.imageExtent = caps.currentExtent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    VkSwapchainKHR sc = VK_NULL_HANDLE;
    VK_OK(vkCreateSwapchainKHR(g_dev, &ci, nullptr, &sc));
    if (!sc) return;
    uint32_t n = 8;
    VkImage images[8];
    VK_OK(vkGetSwapchainImagesKHR(g_dev, sc, &n, images));
    const uint32_t W = caps.currentExtent.width, H = caps.currentExtent.height;
    VkRenderPass rp = makeRenderPass(VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_UNDEFINED);
    std::vector<VkFramebuffer> fbs;
    for (uint32_t i = 0; i < n; i++) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = images[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = VK_FORMAT_B8G8R8A8_UNORM;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView v = VK_NULL_HANDLE;
        VK_OK(vkCreateImageView(g_dev, &vi, nullptr, &v));
        fbs.push_back(makeFramebuffer(rp, {v}, W, H));
    }
    PipelineDesc pd;
    pd.vs = shader(spv_vidx_vert);
    pd.fs = shader(spv_tri_frag);
    pd.layout = pipeLayout(nullptr, VK_SHADER_STAGE_VERTEX_BIT, sizeof(QuadPC));
    pd.rp = rp;
    VkPipeline pipe = makePipeline(pd);
    for (int f = 0; f < frames && pipe; f++) {
        MSG msg;
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
        uint32_t idx = 0;
        VK_OK(vkAcquireNextImageKHR(g_dev, sc, UINT64_MAX, VK_NULL_HANDLE, VK_NULL_HANDLE, &idx));
        begin();
        float t = (float)f / 60.0f;
        beginPass(rp, fbs[idx], W, H, 0.5f + 0.5f * sinf(t), 0.2f, 0.3f);
        vkCmdBindPipeline(g_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        VkViewport vp{W * 0.25f, H * 0.25f, W * 0.5f, H * 0.5f, 0, 1};
        vkCmdSetViewport(g_cmd, 0, 1, &vp);
        drawQuad(pd.layout, 0, 0, 0, 0, 1);
        drawQuad(pd.layout, 1, 0, 1, 0, 0);
        drawQuad(pd.layout, 0, 1, 0, 1, 0);
        drawQuad(pd.layout, 1, 1, 1, 1, 0);
        vkCmdEndRenderPass(g_cmd);
        submit();
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.swapchainCount = 1;
        pi.pSwapchains = &sc;
        pi.pImageIndices = &idx;
        VkResult r = vkQueuePresentKHR(g_queue, &pi);
        check(r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR, "vkQueuePresentKHR returned %d", (int)r);
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) break;
    }
    vkDestroySwapchainKHR(g_dev, sc, nullptr);
    vkDestroySurfaceKHR(g_inst, surf, nullptr);
    DestroyWindow(hwnd);
}

// Measures the driver's own CPU cost per draw call: many tiny draws, each with
// its own push constants, off-screen so the display is not the limit.
static void bench(int drawsPerFrame) {
    printf("[bench] %d draws per frame\n", drawsPerFrame);
    Img rt = makeImage(kColor, 256, 256, kTargetUsage);
    VkRenderPass rp = makeRenderPass(kColor, VK_FORMAT_UNDEFINED);
    VkFramebuffer fb = makeFramebuffer(rp, {rt.v}, 256, 256);
    PipelineDesc pd;
    pd.vs = shader(spv_vidx_vert);
    pd.fs = shader(spv_tri_frag);
    pd.layout = pipeLayout(nullptr, VK_SHADER_STAGE_VERTEX_BIT, sizeof(QuadPC));
    pd.rp = rp;
    VkPipeline pipe = makePipeline(pd);
    if (!pipe) return;
    LARGE_INTEGER freq, t0, t1, t2;
    QueryPerformanceFrequency(&freq);
    double recordMs = 0, submitMs = 0;
    const int frames = 60;
    for (int f = 0; f < frames; f++) {
        QueryPerformanceCounter(&t0);
        begin();
        beginPass(rp, fb, 256, 256, 0, 0, 0);
        vkCmdBindPipeline(g_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        for (int i = 0; i < drawsPerFrame; i++) drawQuad(pd.layout, (float)(i % 16) / 8.0f, (float)((i / 16) % 16) / 8.0f, 1, 0.5f, 0.25f);
        vkCmdEndRenderPass(g_cmd);
        QueryPerformanceCounter(&t1);
        submit();
        QueryPerformanceCounter(&t2);
        recordMs += (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart;
        submitMs += (double)(t2.QuadPart - t1.QuadPart) * 1000.0 / (double)freq.QuadPart;
    }
    double total = recordMs + submitMs;
    printf("    record %.3f ms/frame, submit+execute %.3f ms/frame, %.0f draws/second\n", recordMs / frames, submitMs / frames,
           (double)drawsPerFrame * frames * 1000.0 / total);
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const char* dll = "vkon11.dll";
    int windowFrames = 0, benchDraws = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--window")) windowFrames = 300;
        else if (!strncmp(argv[i], "--window=", 9)) windowFrames = atoi(argv[i] + 9);
        else if (!strcmp(argv[i], "--bench")) benchDraws = 5000;
        else if (!strncmp(argv[i], "--bench=", 8)) benchDraws = atoi(argv[i] + 8);
        else if (!strcmp(argv[i], "--allow-skip")) g_allowSkip = true;
        else dll = argv[i];
    }
    HMODULE mod = LoadLibraryA(dll);
    if (!mod) { printf("cannot load %s (error %lu)\n", dll, GetLastError()); return 2; }
    typedef PFN_vkVoidFunction(VKAPI_CALL * GetProc)(VkInstance, const char*);
    GetProc gpa = (GetProc)(void*)GetProcAddress(mod, "vk_icdGetInstanceProcAddr");
    if (!gpa) { printf("%s does not export vk_icdGetInstanceProcAddr\n", dll); return 2; }
#define X(n) vk##n = (PFN_vk##n)gpa(VK_NULL_HANDLE, "vk" #n); if (!vk##n) { printf("missing entry point vk" #n "\n"); return 2; }
    VK_FUNCS(X)
#undef X

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    const char* iext[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = iext;
    if (vkCreateInstance(&ici, nullptr, &g_inst) != VK_SUCCESS) { printf("vkCreateInstance failed\n"); return 2; }
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(g_inst, &count, nullptr);
    if (!count) { printf("no Direct3D 11 (feature level 11_0) adapter found\n"); return 2; }
    std::vector<VkPhysicalDevice> pds(count);
    vkEnumeratePhysicalDevices(g_inst, &count, pds.data());
    for (uint32_t i = 0; i < count; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pds[i], &p);
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(pds[i], &mp);
        printf("device %u: %s  vendor=0x%04x device=0x%04x  Vulkan %u.%u.%u  VRAM %llu MB\n", i, p.deviceName, p.vendorID, p.deviceID,
               VK_VERSION_MAJOR(p.apiVersion), VK_VERSION_MINOR(p.apiVersion), VK_VERSION_PATCH(p.apiVersion),
               (unsigned long long)(mp.memoryHeaps[0].size >> 20));
    }
    g_pd = pds[0];

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    const char* dext[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_MAINTENANCE_1_EXTENSION_NAME};
    dci.enabledExtensionCount = 2;
    dci.ppEnabledExtensionNames = dext;
    if (vkCreateDevice(g_pd, &dci, nullptr, &g_dev) != VK_SUCCESS) { printf("vkCreateDevice failed\n"); return 2; }
    vkGetDeviceQueue(g_dev, 0, 0, &g_queue);
    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    vkCreateCommandPool(g_dev, &cpi, nullptr, &g_pool);
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = g_pool;
    cai.commandBufferCount = 1;
    vkAllocateCommandBuffers(g_dev, &cai, &g_cmd);
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 64};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 64;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &ps;
    vkCreateDescriptorPool(g_dev, &dpi, nullptr, &g_dpool);

    testClear();
    testTriangle();
    testTextured();
    testVertexIndex();
    testInstancing();
    testTransfers();
    testMsaa();
    testCompute();
    testComputeImage();
    if (windowFrames) testWindow(windowFrames);
    if (benchDraws) bench(benchDraws);

    vkDeviceWaitIdle(g_dev);
    vkDestroyDevice(g_dev, nullptr);
    vkDestroyInstance(g_inst, nullptr);
    printf("\n%d checks, %d failed, %d skipped\n%s\n", g_checks, g_failures, g_skips,
           g_failures ? "SELF-TEST FAILED" : "SELF-TEST PASSED");
    return g_failures ? 1 : 0;
}
