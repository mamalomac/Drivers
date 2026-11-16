// SPIR-V -> HLSL (SPIRV-Cross) -> D3D bytecode (d3dcompiler_47).
#include "vkon11.h"
#include <d3dcommon.h>
#include <spirv_hlsl.hpp>

namespace vkon11 {

static spv::ExecutionModel modelOf(VkShaderStageFlagBits s) {
    switch (s) {
    case VK_SHADER_STAGE_VERTEX_BIT: return spv::ExecutionModelVertex;
    case VK_SHADER_STAGE_FRAGMENT_BIT: return spv::ExecutionModelFragment;
    default: return spv::ExecutionModelGLCompute;
    }
}
static int stageIndex(VkShaderStageFlagBits s) {
    return s == VK_SHADER_STAGE_VERTEX_BIT ? ST_VS : s == VK_SHADER_STAGE_FRAGMENT_BIT ? ST_PS : ST_CS;
}

bool translateShader(const ShaderModule& mod, const char* entry, VkShaderStageFlagBits stage,
                     const PipelineLayoutData& layout, const VkSpecializationInfo* spec, std::string& hlsl,
                     std::string& err) {
    try {
        spirv_cross::CompilerHLSL c(mod.code);
        const spv::ExecutionModel model = modelOf(stage);
        c.set_entry_point(entry, model);

        if (spec) {
            for (const auto& sc : c.get_specialization_constants()) {
                for (uint32_t i = 0; i < spec->mapEntryCount; i++) {
                    const VkSpecializationMapEntry& e = spec->pMapEntries[i];
                    if (e.constantID != sc.constant_id) continue;
                    auto& k = c.get_constant(sc.id);
                    uint64_t v = 0;
                    memcpy(&v, (const uint8_t*)spec->pData + e.offset, std::min<size_t>(e.size, 8));
                    k.m.c[0].r[0].u64 = v;
                }
            }
        }

        auto common = c.get_common_options();
        common.vertex.flip_vert_y = stage == VK_SHADER_STAGE_VERTEX_BIT; // Vulkan NDC is Y-down, D3D is Y-up
        common.vertex.fixup_clipspace = false;                           // both use 0..1 depth
        c.set_common_options(common);

        spirv_cross::CompilerHLSL::Options ho;
        ho.shader_model = 50;
        ho.point_size_compat = true;
        ho.point_coord_compat = true;
        ho.support_nonzero_base_vertex_base_instance = true;
        ho.force_storage_buffer_as_uav = true;
        c.set_hlsl_options(ho);

        const int st = stageIndex(stage);
        for (const SlotMap& s : layout.slots[st]) {
            const SetBinding& b = layout.sets[s.set]->bindings[s.bindingIndex];
            spirv_cross::HLSLResourceBinding rb;
            rb.stage = model;
            rb.desc_set = s.set;
            rb.binding = b.binding;
            rb.cbv.register_binding = s.cbv;
            rb.srv.register_binding = s.srv;
            rb.sampler.register_binding = s.sampler;
            rb.uav.register_binding = s.uav;
            c.add_hlsl_resource_binding(rb);
        }
        spirv_cross::HLSLResourceBinding pc;
        pc.stage = model;
        pc.desc_set = spirv_cross::ResourceBindingPushConstantDescriptorSet;
        pc.binding = spirv_cross::ResourceBindingPushConstantBinding;
        pc.cbv.register_binding = SLOT_PUSH_CONSTANTS;
        c.add_hlsl_resource_binding(pc);
        c.set_hlsl_aux_buffer_binding(spirv_cross::HLSL_AUX_BINDING_BASE_VERTEX_INSTANCE, SLOT_VERTEX_INFO, 0);

        hlsl = c.compile();

        if (stage == VK_SHADER_STAGE_VERTEX_BIT) {
            // Make the Y flip a run-time constant so that negative-height viewports
            // (VK_KHR_maintenance1) can turn it off. It lives in the same cbuffer as
            // SPIRV-Cross' base vertex/instance values (register b12).
            const std::string flip = "gl_Position.y = -gl_Position.y;";
            size_t pos = hlsl.find(flip);
            if (pos != std::string::npos) hlsl.replace(pos, flip.size(), "gl_Position.y = gl_Position.y * vkon11_yflip;");
            const std::string tail = "int SPIRV_Cross_BaseInstance;\n";
            size_t t = hlsl.find(tail);
            if (t != std::string::npos) hlsl.insert(t + tail.size(), "    float vkon11_yflip;\n");
            else
                hlsl = "cbuffer SPIRV_Cross_VertexInfo : register(b" + std::to_string(SLOT_VERTEX_INFO) +
                       ")\n{\n    int vkon11_unusedBaseVertex;\n    int vkon11_unusedBaseInstance;\n    float vkon11_yflip;\n};\n\n" + hlsl;
        }
        return true;
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
}

// ---------------------------------------------------------------------------
// D3D links stages by output/input *register*, and the HLSL compiler packs
// registers according to the full member list. SPIRV-Cross only declares the
// inputs a fragment shader actually reads, so we rewrite the pixel shader's
// input struct to mirror the vertex shader's output struct exactly.
struct Member { std::string mods, type, name, sem; };

static bool parseStruct(const std::string& src, const char* header, size_t& bodyBegin, size_t& bodyEnd,
                        std::vector<Member>& out) {
    size_t h = src.find(header);
    if (h == std::string::npos) return false;
    size_t open = src.find('{', h);
    size_t close = src.find("};", open);
    if (open == std::string::npos || close == std::string::npos) return false;
    bodyBegin = open + 1;
    bodyEnd = close;
    size_t pos = bodyBegin;
    while (pos < bodyEnd) {
        size_t semi = src.find(';', pos);
        if (semi == std::string::npos || semi > bodyEnd) break;
        std::string line = src.substr(pos, semi - pos);
        pos = semi + 1;
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string decl = line.substr(0, colon), sem = line.substr(colon + 1);
        auto trim = [](std::string& s) {
            size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
            s = a == std::string::npos ? "" : s.substr(a, b - a + 1);
        };
        trim(decl);
        trim(sem);
        Member m;
        m.sem = sem;
        size_t sp = decl.find_last_of(" \t");
        if (sp == std::string::npos) continue;
        m.name = decl.substr(sp + 1);
        std::string rest = decl.substr(0, sp);
        trim(rest);
        sp = rest.find_last_of(" \t");
        if (sp == std::string::npos) m.type = rest;
        else { m.type = rest.substr(sp + 1); m.mods = rest.substr(0, sp); trim(m.mods); }
        out.push_back(m);
    }
    return true;
}

void matchStageInterface(const std::string& vsHlsl, std::string& psHlsl) {
    std::vector<Member> vsOut, psIn;
    size_t vb, ve, pb, pe;
    if (!parseStruct(vsHlsl, "struct SPIRV_Cross_Output", vb, ve, vsOut)) return;
    if (!parseStruct(psHlsl, "struct SPIRV_Cross_Input", pb, pe, psIn)) return;
    auto isUser = [](const Member& m) { return m.sem.compare(0, 8, "TEXCOORD") == 0; };
    std::string body = "\n";
    std::vector<bool> used(psIn.size(), false);
    int pad = 0;
    auto emit = [&](const std::string& mods, const std::string& type, const std::string& name, const std::string& sem) {
        body += "    ";
        if (!mods.empty()) body += mods + " ";
        body += type + " " + name + " : " + sem + ";\n";
    };
    for (const Member& v : vsOut) {
        if (!isUser(v)) continue;
        size_t found = psIn.size();
        for (size_t i = 0; i < psIn.size(); i++)
            if (psIn[i].sem == v.sem) { found = i; break; }
        if (found < psIn.size()) {
            used[found] = true;
            emit(psIn[found].mods, v.type, psIn[found].name, v.sem);
        } else {
            emit(v.mods, v.type, "_vkon11_pad" + std::to_string(pad++), v.sem);
        }
    }
    for (size_t i = 0; i < psIn.size(); i++)
        if (!used[i]) emit(psIn[i].mods, psIn[i].type, psIn[i].name, psIn[i].sem);
    psHlsl.replace(pb, pe - pb, body);
}

// ---------------------------------------------------------------------------
typedef HRESULT(WINAPI* D3DCompileFn)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT,
                                      ID3DBlob**, ID3DBlob**);

static D3DCompileFn getCompiler() {
    static D3DCompileFn fn = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        HMODULE m = LoadLibraryA("d3dcompiler_47.dll");
        if (!m) m = LoadLibraryA("d3dcompiler_46.dll");
        if (!m) m = LoadLibraryA("d3dcompiler_43.dll");
        if (m) fn = (D3DCompileFn)(void*)GetProcAddress(m, "D3DCompile");
        if (!fn) VKON_LOG("d3dcompiler_47.dll not found: shaders cannot be compiled");
    }
    return fn;
}

static std::mutex g_cacheMutex;
static std::unordered_map<std::string, std::vector<uint8_t>> g_cache;

// ---------------------------------------------------------------------------
// On-disk bytecode cache. The HLSL compiler takes tens to hundreds of
// milliseconds per shader, which shows up as stutter; compiled bytecode is kept
// in %LOCALAPPDATA%\VKOn11\shadercache so each shader is only compiled once.
// VKON11_SHADER_CACHE=0 disables it, any other value overrides the directory.
static const std::string& cacheDir() {
    static std::string dir;
    static bool init = false;
    if (init) return dir;
    init = true;
    char buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("VKON11_SHADER_CACHE", buf, sizeof(buf));
    if (n > 0 && n < sizeof(buf)) {
        if (strcmp(buf, "0")) dir = buf;
    } else {
        n = GetEnvironmentVariableA("LOCALAPPDATA", buf, sizeof(buf));
        if (n > 0 && n < sizeof(buf) - 32) {
            dir = std::string(buf) + "\\VKOn11";
            CreateDirectoryA(dir.c_str(), nullptr);
            dir += "\\shadercache";
        }
    }
    if (!dir.empty()) {
        CreateDirectoryA(dir.c_str(), nullptr);
        DWORD attr = GetFileAttributesA(dir.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) dir.clear();
    }
    return dir;
}

static uint64_t fnv1a(const std::string& s, uint64_t h) {
    for (unsigned char ch : s) { h ^= ch; h *= 1099511628211ull; }
    return h;
}

static std::string cachePath(const std::string& key) {
    const std::string& dir = cacheDir();
    if (dir.empty()) return std::string();
    char name[48];
    snprintf(name, sizeof(name), "\\%016llx%016llx.dxbc", (unsigned long long)fnv1a(key, 14695981039346656037ull),
             (unsigned long long)fnv1a(key, 0x9e3779b97f4a7c15ull));
    return dir + name;
}

static bool loadCached(const std::string& path, std::vector<uint8_t>& out) {
    if (path.empty()) return false;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    bool ok = false;
    if (size >= 32 && size < (64 << 20)) {
        out.resize((size_t)size);
        uint32_t total = 0;
        ok = fread(out.data(), 1, (size_t)size, f) == (size_t)size && !memcmp(out.data(), "DXBC", 4);
        if (ok) { memcpy(&total, out.data() + 24, 4); ok = total == (uint32_t)size; } // container records its own size
    }
    fclose(f);
    return ok;
}

static void storeCached(const std::string& path, const std::vector<uint8_t>& data) {
    if (path.empty()) return;
    std::string tmp = path + ".tmp" + std::to_string(GetCurrentProcessId());
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return;
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    fclose(f);
    if (!ok || !MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) DeleteFileA(tmp.c_str());
}

bool compileHLSL(const std::string& hlsl, const char* entry, const char* profile, std::vector<uint8_t>& out,
                 std::string& err) {
    std::string key = std::string(profile) + "|" + entry + "|" + hlsl;
    {
        std::lock_guard<std::mutex> lk(g_cacheMutex);
        auto it = g_cache.find(key);
        if (it != g_cache.end()) { out = it->second; return true; }
    }
    const std::string path = cachePath(key);
    if (loadCached(path, out)) {
        std::lock_guard<std::mutex> lk(g_cacheMutex);
        g_cache[key] = out;
        return true;
    }
    D3DCompileFn fn = getCompiler();
    if (!fn) { err = "d3dcompiler not available"; return false; }
    ID3DBlob *code = nullptr, *msgs = nullptr;
    HRESULT hr = fn(hlsl.data(), hlsl.size(), "vkon11", nullptr, nullptr, entry, profile, 0, 0, &code, &msgs);
    if (msgs) {
        if (FAILED(hr)) err.assign((const char*)msgs->GetBufferPointer(), msgs->GetBufferSize());
        msgs->Release();
    }
    if (FAILED(hr) || !code) {
        if (err.empty()) err = "D3DCompile failed";
        if (code) code->Release();
        return false;
    }
    out.assign((const uint8_t*)code->GetBufferPointer(), (const uint8_t*)code->GetBufferPointer() + code->GetBufferSize());
    code->Release();
    storeCached(path, out);
    std::lock_guard<std::mutex> lk(g_cacheMutex);
    g_cache[key] = out;
    return true;
}

} // namespace vkon11
