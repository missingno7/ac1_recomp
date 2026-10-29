#include "ac1_d3d9_bridge.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>
#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>

namespace {
constexpr uint32_t kGuestBase = 0x00010000u;
constexpr uint32_t kGuestLimit = 0x00050000u;
constexpr uint32_t kGuestObjectPool = 0x00020000u;
std::array<uint8_t, kGuestLimit - kGuestBase> guest_memory{};
uint32_t next_guest_address = kGuestObjectPool;
std::unordered_set<uint32_t> live_guest_allocations;
bool fail_next_guest_allocation = false;
HWND hidden_window = nullptr;
std::array<void *, 17> fake_d3d_vtable{};
std::array<void *, 119> fake_device_vtable{};
std::array<void *, 17> fake_surface_vtable{};
std::array<void *, 14> fake_vertex_buffer_vtable{};
std::array<void *, 22> fake_cube_texture_vtable{};
std::array<void *, 5> fake_pixel_shader_vtable{};
std::array<void *, 5> fake_vertex_shader_vtable{};

void *resolve_guest(void *context, uint32_t address, uint32_t size, uint32_t access);
uint32_t allocate_guest(void *context, uint32_t size, uint32_t alignment);
uint32_t guest_word(uint32_t address);
uint32_t object_slot(uint32_t object, uint32_t slot);

bool compile_native_smoke_shader(const char *source, const char *profile,
                                 std::vector<uint8_t> *bytecode)
{
    if (!source || !profile || !bytecode) return false;
    HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    if (!compiler) return false;
    auto compile = reinterpret_cast<pD3DCompile>(GetProcAddress(compiler, "D3DCompile"));
    if (!compile) {
        FreeLibrary(compiler);
        return false;
    }
    ID3DBlob *shader = nullptr;
    ID3DBlob *errors = nullptr;
    const HRESULT status = compile(source, std::strlen(source), "ac1-d3d9-smoke",
        nullptr, nullptr, "main", profile, 0, 0, &shader, &errors);
    if (FAILED(status) && errors)
        std::fprintf(stderr, "D3DCompile diagnostic: %s\n",
                     static_cast<const char *>(errors->GetBufferPointer()));
    bool ok = SUCCEEDED(status) && shader && shader->GetBufferSize() >= sizeof(DWORD);
    if (ok) {
        const auto *begin = static_cast<const uint8_t *>(shader->GetBufferPointer());
        bytecode->assign(begin, begin + shader->GetBufferSize());
    }
    if (shader) shader->Release();
    if (errors) errors->Release();
    FreeLibrary(compiler);
    return ok;
}

bool run_native_shader_smoke(uint32_t device, const char *source,
                             const char *profile, uint32_t create_slot,
                             uint32_t bind_slot, const char *label)
{
    std::vector<uint8_t> bytecode;
    if (!compile_native_smoke_shader(source, profile, &bytecode)) {
        std::fprintf(stderr, "could not compile synthetic %s smoke shader\n", profile);
        return false;
    }
    const uint32_t code_address = allocate_guest(
        nullptr, static_cast<uint32_t>(bytecode.size()), 4);
    const uint32_t shader_output = allocate_guest(nullptr, sizeof(uint32_t), 4);
    std::memcpy(resolve_guest(nullptr, code_address,
        static_cast<uint32_t>(bytecode.size()), AC1_D3D9_GUEST_WRITE),
        bytecode.data(), bytecode.size());
    const uint32_t create_args[] = {device, code_address, shader_output};
    uint32_t result = 0;
    if (ac1_d3d9_dispatch(object_slot(device, create_slot), create_args, 3,
                          &result) != 1 || FAILED(static_cast<HRESULT>(result)) ||
        !guest_word(shader_output)) {
        std::fprintf(stderr, "native DXVK Create%sShader smoke failed: 0x%08x %s\n",
                     label, result, ac1_d3d9_last_error());
        return false;
    }

    const uint32_t shader = guest_word(shader_output);
    const uint32_t bind_args[] = {device, shader};
    const uint32_t clear_args[] = {device, 0};
    const uint32_t release_args[] = {shader};
    if (ac1_d3d9_dispatch(object_slot(device, bind_slot), bind_args, 2,
                          &result) != 1 || FAILED(static_cast<HRESULT>(result)) ||
        ac1_d3d9_dispatch(object_slot(device, bind_slot), clear_args, 2,
                          &result) != 1 || FAILED(static_cast<HRESULT>(result)) ||
        ac1_d3d9_dispatch(object_slot(shader, 2), release_args, 1, &result) != 1 ||
        result != 0) {
        std::fprintf(stderr, "native DXVK %s-shader bind/release smoke failed: %s\n",
                     label, ac1_d3d9_last_error());
        return false;
    }
    std::printf("native DXVK %s-shader create/bind/unbind/release passed\n", label);
    return true;
}

bool run_native_indexed_triangle_smoke(uint32_t device, uint32_t vertex_buffer,
                                      uint32_t index_buffer, uint32_t texture)
{
    uint32_t result = 0;
    auto call = [&](uint32_t slot, const uint32_t *args, uint32_t count,
                    const char *name) {
        if (ac1_d3d9_argument_count(object_slot(device, slot)) !=
                static_cast<int>(count) ||
            ac1_d3d9_dispatch(object_slot(device, slot), args, count, &result) != 1 ||
            FAILED(static_cast<HRESULT>(result))) {
            std::fprintf(stderr, "synthetic triangle %s failed: hr=0x%08x %s\n",
                         name, result, ac1_d3d9_last_error());
            return false;
        }
        return true;
    };
    struct Vertex { float x, y, z; uint32_t color; };
    const Vertex vertices[] = {
        {-0.70f, -0.70f, 0.5f, 0xffff2020u},
        { 0.00f,  0.70f, 0.5f, 0xffff2020u},
        { 0.70f, -0.70f, 0.5f, 0xffff2020u}
    };
    const uint16_t indices[] = {0, 1, 2};
    const uint32_t vertex_lock_output = allocate_guest(nullptr, 4, 4);
    const uint32_t vertex_lock[] = {
        vertex_buffer, 0u, sizeof(vertices), vertex_lock_output, D3DLOCK_DISCARD
    };
    if (ac1_d3d9_dispatch(object_slot(vertex_buffer, 11), vertex_lock, 5,
                          &result) != 1 || FAILED(static_cast<HRESULT>(result))) {
        std::fprintf(stderr, "synthetic triangle VB Lock failed: 0x%08x %s\n",
                     result, ac1_d3d9_last_error());
        return false;
    }
    const uint32_t vertex_guest = guest_word(vertex_lock_output);
    void *vertex_data = resolve_guest(nullptr, vertex_guest, sizeof(vertices),
                                      AC1_D3D9_GUEST_WRITE);
    if (!vertex_data) return false;
    std::memcpy(vertex_data, vertices, sizeof(vertices));
    const uint32_t vertex_unlock[] = {vertex_buffer};
    if (ac1_d3d9_dispatch(object_slot(vertex_buffer, 12), vertex_unlock, 1,
                          &result) != 1 || FAILED(static_cast<HRESULT>(result))) {
        std::fprintf(stderr, "synthetic triangle VB Unlock failed: 0x%08x %s\n",
                     result, ac1_d3d9_last_error());
        return false;
    }
    const uint32_t index_lock_output = allocate_guest(nullptr, 4, 4);
    const uint32_t index_lock[] = {
        index_buffer, 0u, sizeof(indices), index_lock_output, D3DLOCK_DISCARD
    };
    if (ac1_d3d9_dispatch(object_slot(index_buffer, 11), index_lock, 5,
                          &result) != 1 || FAILED(static_cast<HRESULT>(result))) {
        std::fprintf(stderr, "synthetic triangle IB Lock failed: 0x%08x %s\n",
                     result, ac1_d3d9_last_error());
        return false;
    }
    const uint32_t index_guest = guest_word(index_lock_output);
    void *index_data = resolve_guest(nullptr, index_guest, sizeof(indices),
                                     AC1_D3D9_GUEST_WRITE);
    if (!index_data) return false;
    std::memcpy(index_data, indices, sizeof(indices));
    const uint32_t index_unlock[] = {index_buffer};
    if (ac1_d3d9_dispatch(object_slot(index_buffer, 12), index_unlock, 1,
                          &result) != 1 || FAILED(static_cast<HRESULT>(result))) {
        std::fprintf(stderr, "synthetic triangle IB Unlock failed: 0x%08x %s\n",
                     result, ac1_d3d9_last_error());
        return false;
    }

    const D3DVERTEXELEMENT9 elements[] = {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT,
         D3DDECLUSAGE_POSITION, 0},
        {0, 12, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT,
         D3DDECLUSAGE_COLOR, 0},
        D3DDECL_END()
    };
    const uint32_t elements_guest = allocate_guest(nullptr, sizeof(elements), 4);
    const uint32_t declaration_output = allocate_guest(nullptr, 4, 4);
    std::memcpy(resolve_guest(nullptr, elements_guest, sizeof(elements),
                              AC1_D3D9_GUEST_WRITE), elements, sizeof(elements));
    const uint32_t create_declaration[] = {device, elements_guest, declaration_output};
    if (!call(86, create_declaration, 3, "CreateVertexDeclaration")) return false;
    const uint32_t declaration = guest_word(declaration_output);
    if (!declaration) return false;

    uint32_t cube_output = allocate_guest(nullptr, 4, 4);
    const uint32_t create_cube[] = {
        device, 4u, 1u, 0u, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, cube_output, 0u
    };
    if (!call(25, create_cube, 8, "CreateCubeTexture")) return false;
    const uint32_t cube = guest_word(cube_output);
    const uint32_t set_2d_texture[] = {device, 0u, texture};
    const uint32_t set_cube_texture[] = {device, 1u, cube};
    const uint32_t clear_2d_texture[] = {device, 0u, 0u};
    const uint32_t clear_cube_texture[] = {device, 1u, 0u};
    if (!call(65, set_2d_texture, 3, "SetTexture(2D proxy)") ||
        !call(65, clear_2d_texture, 3, "SetTexture(2D unbind)") ||
        !call(65, set_cube_texture, 3, "SetTexture(cube proxy)") ||
        !call(65, clear_cube_texture, 3, "SetTexture(cube unbind)")) return false;

    const uint32_t query_probe[] = {device, D3DQUERYTYPE_OCCLUSION, 0u};
    if (ac1_d3d9_argument_count(object_slot(device, 118)) != 3 ||
        ac1_d3d9_dispatch(object_slot(device, 118), query_probe, 3, &result) != 1 ||
        FAILED(static_cast<HRESULT>(result))) {
        std::fprintf(stderr, "native DXVK occlusion CreateQuery capability probe failed: 0x%08x %s\n",
                     result, ac1_d3d9_last_error());
        return false;
    }
    const uint32_t query_output = allocate_guest(nullptr, 4, 4);
    const uint32_t create_query[] = {device, D3DQUERYTYPE_OCCLUSION, query_output};
    if (!call(118, create_query, 3, "CreateQuery(occlusion)")) return false;
    const uint32_t query = guest_word(query_output);
    if (!query || ac1_d3d9_argument_count(object_slot(query, 6)) != 2 ||
        ac1_d3d9_argument_count(object_slot(query, 7)) != 4 ||
        ac1_d3d9_dispatch(object_slot(query, 4), &query, 1, &result) != 1 ||
        result != D3DQUERYTYPE_OCCLUSION ||
        ac1_d3d9_dispatch(object_slot(query, 5), &query, 1, &result) != 1 ||
        result != sizeof(DWORD)) {
        std::fprintf(stderr, "native occlusion query proxy metadata/ABI failed: %s\n",
                     ac1_d3d9_last_error());
        return false;
    }

    const uint32_t bind_stream[] = {device, 0u, vertex_buffer, 0u, sizeof(Vertex)};
    const uint32_t bind_indices[] = {device, index_buffer};
    const uint32_t bind_declaration[] = {device, declaration};
    const uint32_t unbind_stream[] = {device, 0u, 0u, 0u, 0u};
    const uint32_t unbind_indices[] = {device, 0u};
    const uint32_t unbind_declaration[] = {device, 0u};
    const uint32_t disable_depth[] = {device, D3DRS_ZENABLE, FALSE};
    const uint32_t disable_cull[] = {device, D3DRS_CULLMODE, D3DCULL_NONE};
    const uint32_t diffuse_op[] = {device, 0u, D3DTSS_COLOROP, D3DTOP_SELECTARG1};
    const uint32_t diffuse_arg[] = {device, 0u, D3DTSS_COLORARG1, D3DTA_DIFFUSE};
    if (!call(57, disable_depth, 3, "SetRenderState(ZENABLE)") ||
        !call(57, disable_cull, 3, "SetRenderState(CULLMODE)") ||
        !call(67, diffuse_op, 4, "SetTextureStageState(COLOROP)") ||
        !call(67, diffuse_arg, 4, "SetTextureStageState(COLORARG1)") ||
        !call(100, bind_stream, 5, "SetStreamSource") ||
        !call(104, bind_indices, 2, "SetIndices") ||
        !call(87, bind_declaration, 2, "SetVertexDeclaration")) return false;

    const uint32_t begin[] = {device};
    const uint32_t query_begin[] = {query, D3DISSUE_BEGIN};
    const uint32_t query_end[] = {query, D3DISSUE_END};
    const uint32_t draw[] = {device, D3DPT_TRIANGLELIST, 0u, 1u};
    const uint32_t draw_indexed[] = {device, D3DPT_TRIANGLELIST, 0u, 0u, 3u, 0u, 1u};
    const uint32_t end[] = {device};
    const uint32_t present[] = {device, 0u, 0u, 0u, 0u};
    if (!call(41, begin, 1, "BeginScene") ||
        ac1_d3d9_dispatch(object_slot(query, 6), query_begin, 2, &result) != 1 ||
        FAILED(static_cast<HRESULT>(result)) ||
        !call(81, draw, 4, "DrawPrimitive") ||
        !call(82, draw_indexed, 7, "DrawIndexedPrimitive") ||
        ac1_d3d9_dispatch(object_slot(query, 6), query_end, 2, &result) != 1 ||
        FAILED(static_cast<HRESULT>(result)) ||
        !call(42, end, 1, "EndScene") ||
        !call(17, present, 5, "Present")) return false;

    const uint32_t query_poll[] = {query, 0u, 0u, D3DGETDATA_FLUSH};
    if (ac1_d3d9_dispatch(object_slot(query, 7), query_poll, 4, &result) != 1 ||
        (static_cast<HRESULT>(result) != S_OK &&
         static_cast<HRESULT>(result) != S_FALSE)) {
        std::fprintf(stderr, "native occlusion query GetData(NULL,0,FLUSH) failed: 0x%08x %s\n",
                     result, ac1_d3d9_last_error());
        return false;
    }

    call(87, unbind_declaration, 2, "SetVertexDeclaration(null)");
    call(104, unbind_indices, 2, "SetIndices(null)");
    call(100, unbind_stream, 5, "SetStreamSource(null)");
    call(65, clear_2d_texture, 3, "SetTexture(null)");
    const uint32_t release_decl[] = {declaration};
    const uint32_t release_cube[] = {cube};
    const uint32_t release_query[] = {query};
    ac1_d3d9_dispatch(object_slot(declaration, 2), release_decl, 1, &result);
    ac1_d3d9_dispatch(object_slot(cube, 2), release_cube, 1, &result);
    ac1_d3d9_dispatch(object_slot(query, 2), release_query, 1, &result);
    std::printf("native DXVK synthetic indexed-triangle and occlusion-query path passed (no readback)\n");
    return true;
}

std::string guest_string(uint32_t address, uint32_t maximum)
{
    std::string result;
    for (uint32_t i = 0; i < maximum; ++i) {
        const auto *character = static_cast<const char *>(resolve_guest(
            nullptr, address + i, 1, AC1_D3D9_GUEST_READ));
        if (!character) return std::string();
        if (!*character) return result;
        result.push_back(*character);
    }
    return std::string();
}

bool run_constant_table_smoke()
{
    static const char source[] =
        "float4 g_ElapsedTime : register(c0);\n"
        "sampler2D s0 : register(s0);\n"
        "sampler2D s1 : register(s1);\n"
        "float4 main(float2 uv : TEXCOORD0) : COLOR0 {\n"
        " return g_ElapsedTime + tex2D(s0, uv) + tex2D(s1, uv);\n"
        "}";
    std::vector<uint8_t> bytecode;
    if (!compile_native_smoke_shader(source, "ps_3_0", &bytecode)) return false;
    const uint32_t code_address = allocate_guest(
        nullptr, static_cast<uint32_t>(bytecode.size()), 4);
    const uint32_t table_output = allocate_guest(nullptr, sizeof(uint32_t), 4);
    std::memcpy(resolve_guest(nullptr, code_address,
        static_cast<uint32_t>(bytecode.size()), AC1_D3D9_GUEST_WRITE),
        bytecode.data(), bytecode.size());
    uint32_t native_result = 0;
    if (ac1_d3d9_get_shader_constant_table(code_address, table_output,
                                            &native_result) != 1 ||
        FAILED(static_cast<HRESULT>(native_result)) || !guest_word(table_output)) {
        std::fprintf(stderr, "D3DXGetShaderConstantTable smoke failed: 0x%08x %s\n",
                     native_result, ac1_d3d9_last_error());
        return false;
    }
    const uint32_t table = guest_word(table_output);
    const uint32_t table_desc_output = allocate_guest(nullptr, 12, 4);
    const uint32_t get_desc_args[] = {table, table_desc_output};
    if (ac1_d3d9_argument_count(object_slot(table, 5)) != 2 ||
        ac1_d3d9_dispatch(object_slot(table, 5), get_desc_args, 2,
                          &native_result) != 1 ||
        FAILED(static_cast<HRESULT>(native_result))) {
        std::fprintf(stderr, "ID3DXConstantTable::GetDesc smoke failed: %s\n",
                     ac1_d3d9_last_error());
        return false;
    }
    uint32_t creator_address = 0, constant_count = 0;
    std::memcpy(&creator_address,
        resolve_guest(nullptr, table_desc_output, 4, AC1_D3D9_GUEST_READ), 4);
    std::memcpy(&constant_count,
        resolve_guest(nullptr, table_desc_output + 8, 4, AC1_D3D9_GUEST_READ), 4);
    if (!creator_address || constant_count != 3 || guest_string(creator_address, 256).empty()) {
        std::fprintf(stderr, "D3DX constant-table descriptor fields mismatch\n");
        return false;
    }

    const uint32_t handle_tokens[] = {object_slot(table, 8), object_slot(table, 8),
                                      object_slot(table, 8)};
    const uint32_t descriptor_address = allocate_guest(nullptr, 48, 4);
    const uint32_t count_address = allocate_guest(nullptr, 4, 4);
    bool saw_elapsed = false, saw_sampler0 = false, saw_sampler1 = false;
    for (uint32_t i = 0; i < constant_count; ++i) {
        const uint32_t get_handle_args[] = {table, 0, i};
        uint32_t handle = 0;
        if (ac1_d3d9_argument_count(handle_tokens[i]) != 3 ||
            ac1_d3d9_dispatch(handle_tokens[i], get_handle_args, 3, &handle) != 1 ||
            !handle) {
            std::fprintf(stderr, "ID3DXConstantTable::GetConstant[%u] failed: %s\n",
                         i, ac1_d3d9_last_error());
            return false;
        }
        if (i == 0) {
            const uint32_t sentinel = 0xdeadbeefu;
            std::memcpy(resolve_guest(nullptr, descriptor_address, 4,
                                      AC1_D3D9_GUEST_WRITE), &sentinel, 4);
            const uint32_t invalid_count_args[] = {
                table, handle, descriptor_address, kGuestLimit - 2
            };
            if (ac1_d3d9_dispatch(object_slot(table, 6), invalid_count_args, 4,
                                  &native_result) != 1 ||
                native_result != static_cast<uint32_t>(D3DERR_INVALIDCALL) ||
                guest_word(descriptor_address) != sentinel) {
                std::fprintf(stderr, "unmapped constant-desc count output was not rejected before write\n");
                return false;
            }
        }
        const uint32_t capacity = 1;
        std::memcpy(resolve_guest(nullptr, count_address, 4, AC1_D3D9_GUEST_WRITE),
                    &capacity, 4);
        const uint32_t get_constant_desc_args[] = {
            table, handle, descriptor_address, count_address
        };
        if (ac1_d3d9_argument_count(object_slot(table, 6)) != 4 ||
            ac1_d3d9_dispatch(object_slot(table, 6), get_constant_desc_args, 4,
                              &native_result) != 1 ||
            FAILED(static_cast<HRESULT>(native_result)) || guest_word(count_address) != 1) {
            std::fprintf(stderr, "ID3DXConstantTable::GetConstantDesc[%u] failed: %s\n",
                         i, ac1_d3d9_last_error());
            return false;
        }
        uint32_t name_address = 0, register_index = 0;
        uint32_t register_set = 0;
        std::memcpy(&name_address,
            resolve_guest(nullptr, descriptor_address, 4, AC1_D3D9_GUEST_READ), 4);
        std::memcpy(&register_set,
            resolve_guest(nullptr, descriptor_address + 4, 4, AC1_D3D9_GUEST_READ), 4);
        std::memcpy(&register_index,
            resolve_guest(nullptr, descriptor_address + 8, 4, AC1_D3D9_GUEST_READ), 4);
        const std::string name = guest_string(name_address, 256);
        saw_elapsed |= name == "g_ElapsedTime" && register_set == 2 && register_index == 0;
        saw_sampler0 |= name == "s0" && register_set == 3 && register_index == 0;
        saw_sampler1 |= name == "s1" && register_set == 3 && register_index == 1;
    }
    const uint32_t release_args[] = {table};
    if (!saw_elapsed || !saw_sampler0 || !saw_sampler1 ||
        ac1_d3d9_dispatch(object_slot(table, 2), release_args, 1,
                          &native_result) != 1 || native_result != 0) {
        std::fprintf(stderr, "D3DX constant metadata or table release mismatch: %s\n",
                     ac1_d3d9_last_error());
        return false;
    }
    std::printf("D3DX constant-table GetDesc/GetConstant/GetConstantDesc passed\n");
    return true;
}

struct GuestPresentParameters {
    uint32_t back_buffer_width;
    uint32_t back_buffer_height;
    uint32_t back_buffer_format;
    uint32_t back_buffer_count;
    uint32_t multi_sample_type;
    uint32_t multi_sample_quality;
    uint32_t swap_effect;
    uint32_t device_window;
    int32_t windowed;
    int32_t enable_auto_depth_stencil;
    uint32_t auto_depth_stencil_format;
    uint32_t flags;
    uint32_t fullscreen_refresh_rate;
    uint32_t presentation_interval;
};
static_assert(sizeof(GuestPresentParameters) == 56, "guest D3D9 ABI test layout");

struct GuestLockedRect {
    int32_t pitch;
    uint32_t bits;
};
static_assert(sizeof(GuestLockedRect) == 8, "guest D3DLOCKED_RECT test layout");

struct FakeComObject {
    void **vtable;
    ULONG references;
    D3DSURFACE_DESC description;
};
FakeComObject fake_d3d{};
FakeComObject fake_device{};
FakeComObject fake_render_target{};
FakeComObject fake_depth_surface{};
FakeComObject fake_vertex_buffer{};
std::array<uint8_t, 64> fake_vertex_buffer_bytes{};
uint32_t fake_vertex_buffer_size = 64;
bool fake_vertex_buffer_locked = false;
IDirect3DVertexBuffer9 *fake_bound_vertex_buffer = nullptr;
FakeComObject fake_cube_texture{};
FakeComObject fake_pixel_shader{};
FakeComObject fake_vertex_shader{};
uint32_t fake_query_calls = 0;
uint32_t fake_render_state_type = UINT32_MAX;
uint32_t fake_render_state_value = UINT32_MAX;
uint32_t fake_texture_stage = UINT32_MAX;
bool fake_texture_was_null = false;
uint32_t fake_texture_stage_type = UINT32_MAX;
uint32_t fake_texture_stage_value = UINT32_MAX;
uint32_t fake_sampler = UINT32_MAX;
uint32_t fake_sampler_type = UINT32_MAX;
uint32_t fake_sampler_value = UINT32_MAX;
uint32_t fake_null_shader_calls = 0;
bool fake_pixel_shader_stream_ok = false;
uint32_t fake_pixel_shader_create_calls = 0;
bool fake_vertex_shader_stream_ok = false;
uint32_t fake_vertex_shader_create_calls = 0;
uint32_t fake_constant_kind = 0;
uint32_t fake_constant_start = UINT32_MAX;
uint32_t fake_constant_count = UINT32_MAX;
std::array<uint32_t, 16> fake_constant_words{};
HRESULT fake_cube_create_result = S_OK;
uint32_t fake_cube_create_calls = 0;
std::array<uint32_t, 6> fake_cube_create_args{};
std::vector<uint32_t> freed_guest_allocations;
uint32_t fake_begin_scene_calls = 0;
uint32_t fake_end_scene_calls = 0;
uint32_t fake_present_calls = 0;
uint32_t fake_draw_primitive_calls = 0;
uint32_t fake_draw_indexed_calls = 0;
uint32_t fake_set_depth_surface_calls = 0;
bool fake_depth_surface_was_null = false;
uint32_t fake_set_render_target_calls = 0;
uint32_t fake_render_target_index = UINT32_MAX;
bool fake_render_target_was_null = false;
HRESULT fake_present_result = S_OK;
bool fake_present_had_source = false;
bool fake_present_had_destination = false;
uintptr_t fake_present_hwnd = 0;
uint32_t fake_present_region_count = UINT32_MAX;
RECT fake_present_source{};
RECT fake_present_destination{};
RECT fake_present_first_dirty_rect{};

HRESULT STDMETHODCALLTYPE fake_query_interface(IUnknown *self, REFIID iid, void **output)
{
    static const GUID iid_unknown =
        {0x00000000u, 0x0000u, 0x0000u, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
    if (!output) return E_POINTER;
    *output = nullptr;
    ++fake_query_calls;
    if (std::memcmp(&iid, &iid_unknown, sizeof(iid_unknown)) != 0) return E_NOINTERFACE;
    ++reinterpret_cast<FakeComObject *>(self)->references;
    *output = self;
    return S_OK;
}

ULONG STDMETHODCALLTYPE fake_add_ref(IUnknown *self)
{
    return ++reinterpret_cast<FakeComObject *>(self)->references;
}

ULONG STDMETHODCALLTYPE fake_release(IUnknown *self)
{
    FakeComObject *object = reinterpret_cast<FakeComObject *>(self);
    return object->references ? --object->references : 0;
}

HRESULT STDMETHODCALLTYPE fake_create_device(
    IDirect3D9 *, UINT, D3DDEVTYPE, HWND, DWORD,
    D3DPRESENT_PARAMETERS *, IDirect3DDevice9 **output)
{
    if (!output) return E_POINTER;
    fake_device.references = 1;
    *output = reinterpret_cast<IDirect3DDevice9 *>(&fake_device);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE fake_create_vertex_buffer(IDirect3DDevice9 *, UINT length,
    DWORD, DWORD, D3DPOOL, IDirect3DVertexBuffer9 **output, HANDLE *shared_handle)
{
    if (!output || shared_handle || !length || length > fake_vertex_buffer_bytes.size())
        return D3DERR_INVALIDCALL;
    fake_vertex_buffer.references = 1;
    fake_vertex_buffer_size = length;
    fake_vertex_buffer_locked = false;
    fake_vertex_buffer_bytes.fill(0);
    *output = reinterpret_cast<IDirect3DVertexBuffer9 *>(&fake_vertex_buffer);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE fake_vertex_buffer_lock(IDirect3DVertexBuffer9 *self,
    UINT offset, UINT size, void **data, DWORD)
{
    if (self != reinterpret_cast<IDirect3DVertexBuffer9 *>(&fake_vertex_buffer) ||
        !data || fake_vertex_buffer_locked || !size ||
        offset > fake_vertex_buffer_size || size > fake_vertex_buffer_size - offset)
        return D3DERR_INVALIDCALL;
    fake_vertex_buffer_locked = true;
    *data = fake_vertex_buffer_bytes.data() + offset;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE fake_vertex_buffer_unlock(IDirect3DVertexBuffer9 *self)
{
    if (self != reinterpret_cast<IDirect3DVertexBuffer9 *>(&fake_vertex_buffer) ||
        !fake_vertex_buffer_locked) return D3DERR_INVALIDCALL;
    fake_vertex_buffer_locked = false;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE fake_vertex_buffer_get_desc(IDirect3DVertexBuffer9 *self,
    D3DVERTEXBUFFER_DESC *description)
{
    if (self != reinterpret_cast<IDirect3DVertexBuffer9 *>(&fake_vertex_buffer) ||
        !description) return D3DERR_INVALIDCALL;
    std::memset(description, 0, sizeof(*description));
    description->Format = D3DFMT_VERTEXDATA;
    description->Type = D3DRTYPE_VERTEXBUFFER;
    description->Usage = D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY;
    description->Pool = D3DPOOL_DEFAULT;
    description->Size = fake_vertex_buffer_size;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE fake_set_stream_source(IDirect3DDevice9 *, UINT stream,
    IDirect3DVertexBuffer9 *buffer, UINT, UINT)
{
    if (stream != 0u || (buffer && buffer != reinterpret_cast<IDirect3DVertexBuffer9 *>(
            &fake_vertex_buffer))) return D3DERR_INVALIDCALL;
    if (buffer) buffer->AddRef();
    if (fake_bound_vertex_buffer) fake_bound_vertex_buffer->Release();
    fake_bound_vertex_buffer = buffer;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE fake_create_cube_texture(
    IDirect3DDevice9 *, UINT edge_length, UINT levels, DWORD usage,
    D3DFORMAT format, D3DPOOL pool, IDirect3DCubeTexture9 **output,
    HANDLE *shared_handle)
{
    ++fake_cube_create_calls;
    fake_cube_create_args = {edge_length, levels, usage,
        static_cast<uint32_t>(format), static_cast<uint32_t>(pool),
        shared_handle ? 1u : 0u};
    if (FAILED(fake_cube_create_result)) return fake_cube_create_result;
    if (!output) return D3DERR_INVALIDCALL;
    fake_cube_texture.references = 1;
    *output = reinterpret_cast<IDirect3DCubeTexture9 *>(&fake_cube_texture);
    return fake_cube_create_result;
}

uintptr_t fake_translate_window(void *, uint32_t guest_hwnd)
{
    return guest_hwnd == 0x1234u ? 1u : 0u;
}

HRESULT STDMETHODCALLTYPE fake_get_render_target(
    IDirect3DDevice9 *, DWORD index, IDirect3DSurface9 **output)
{
    if (!output) return E_POINTER;
    *output = nullptr;
    if (index != 0) return D3DERR_INVALIDCALL;
    ++fake_render_target.references;
    *output = reinterpret_cast<IDirect3DSurface9 *>(&fake_render_target);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE fake_set_render_target(IDirect3DDevice9 *, DWORD index,
                                                   IDirect3DSurface9 *surface)
{
    ++fake_set_render_target_calls;
    fake_render_target_index = index;
    fake_render_target_was_null = surface == nullptr;
    if (surface && surface != reinterpret_cast<IDirect3DSurface9 *>(&fake_render_target))
        return E_INVALIDARG;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE fake_get_depth_surface(
    IDirect3DDevice9 *, IDirect3DSurface9 **output)
{
    if (!output) return E_POINTER;
    ++fake_depth_surface.references;
    *output = reinterpret_cast<IDirect3DSurface9 *>(&fake_depth_surface);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE fake_set_depth_surface(IDirect3DDevice9 *,
                                                   IDirect3DSurface9 *surface)
{
    ++fake_set_depth_surface_calls;
    fake_depth_surface_was_null = surface == nullptr;
    if (surface && surface != reinterpret_cast<IDirect3DSurface9 *>(&fake_depth_surface))
        return E_INVALIDARG;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE fake_set_render_state(IDirect3DDevice9 *,
                                                  D3DRENDERSTATETYPE state,
                                                  DWORD value)
{
    fake_render_state_type = static_cast<uint32_t>(state);
    fake_render_state_value = value;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE fake_test_cooperative_level(IDirect3DDevice9 *)
{
    return D3DERR_DEVICELOST;
}

HRESULT STDMETHODCALLTYPE fake_begin_scene(IDirect3DDevice9 *)
{
    ++fake_begin_scene_calls;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE fake_end_scene(IDirect3DDevice9 *)
{
    ++fake_end_scene_calls;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE fake_present(IDirect3DDevice9 *, const RECT *source,
                                        const RECT *destination, HWND window,
                                        const RGNDATA *dirty_region)
{
    ++fake_present_calls;
    fake_present_had_source = source != nullptr;
    fake_present_had_destination = destination != nullptr;
    if (source) fake_present_source = *source;
    if (destination) fake_present_destination = *destination;
    fake_present_hwnd = reinterpret_cast<uintptr_t>(window);
    fake_present_region_count = dirty_region ? dirty_region->rdh.nCount : UINT32_MAX;
    if (dirty_region && dirty_region->rdh.nCount) {
        std::memcpy(&fake_present_first_dirty_rect, dirty_region->Buffer,
                    sizeof(fake_present_first_dirty_rect));
    }
    return fake_present_result;
}

HRESULT STDMETHODCALLTYPE fake_draw_primitive(IDirect3DDevice9 *, D3DPRIMITIVETYPE,
                                                UINT, UINT)
{
    ++fake_draw_primitive_calls;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE fake_draw_indexed_primitive(IDirect3DDevice9 *,
    D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT)
{
    ++fake_draw_indexed_calls;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE fake_get_device_caps(IDirect3DDevice9 *, D3DCAPS9 *output)
{
    if (!output) return D3DERR_INVALIDCALL;
    std::memset(output, 0, sizeof(*output));
    output->MaxVertexShaderConst = 256;
    output->PixelShaderVersion = D3DPS_VERSION(3, 0);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE fake_set_texture(IDirect3DDevice9 *, DWORD stage,
                                             IDirect3DBaseTexture9 *texture)
{
    fake_texture_stage = stage;
    fake_texture_was_null = texture == nullptr;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE fake_set_texture_stage_state(IDirect3DDevice9 *, DWORD stage,
    D3DTEXTURESTAGESTATETYPE state, DWORD value)
{
    fake_texture_stage = stage;
    fake_texture_stage_type = static_cast<uint32_t>(state);
    fake_texture_stage_value = value;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE fake_set_sampler_state(IDirect3DDevice9 *, DWORD sampler,
    D3DSAMPLERSTATETYPE state, DWORD value)
{
    fake_sampler = sampler;
    fake_sampler_type = static_cast<uint32_t>(state);
    fake_sampler_value = value;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE fake_set_vertex_shader(IDirect3DDevice9 *,
                                                  IDirect3DVertexShader9 *shader)
{
    if (shader && shader != reinterpret_cast<IDirect3DVertexShader9 *>(
            &fake_vertex_shader)) return E_INVALIDARG;
    ++fake_null_shader_calls;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE fake_set_pixel_shader(IDirect3DDevice9 *,
                                                 IDirect3DPixelShader9 *shader)
{
    if (shader && shader != reinterpret_cast<IDirect3DPixelShader9 *>(
            &fake_pixel_shader)) return E_INVALIDARG;
    ++fake_null_shader_calls;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE fake_create_pixel_shader(IDirect3DDevice9 *,
    const DWORD *bytecode, IDirect3DPixelShader9 **output)
{
    ++fake_pixel_shader_create_calls;
    fake_pixel_shader_stream_ok = bytecode && output &&
        bytecode[0] == D3DPS_VERSION(3, 0) &&
        bytecode[1] == (51u << D3DSI_COMMENTSIZE_SHIFT | D3DSIO_COMMENT) &&
        bytecode[2] == D3DSIO_END && bytecode[53] == (3u << 24 | 1u) &&
        bytecode[54] == D3DSIO_END && bytecode[109] == D3DSIO_END;
    if (!output) return E_POINTER;
    fake_pixel_shader.references = 1;
    *output = reinterpret_cast<IDirect3DPixelShader9 *>(&fake_pixel_shader);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE fake_create_vertex_shader(IDirect3DDevice9 *,
    const DWORD *bytecode, IDirect3DVertexShader9 **output)
{
    ++fake_vertex_shader_create_calls;
    fake_vertex_shader_stream_ok = bytecode && output &&
        bytecode[0] == D3DVS_VERSION(3, 0) &&
        bytecode[1] == D3DSIO_NOP && bytecode[2] == D3DSIO_END;
    if (!output) return E_POINTER;
    fake_vertex_shader.references = 1;
    *output = reinterpret_cast<IDirect3DVertexShader9 *>(&fake_vertex_shader);
    return S_OK;
}

void capture_constants(uint32_t kind, UINT start, const void *values,
                       UINT count, uint32_t bytes_per_register)
{
    fake_constant_kind = kind;
    fake_constant_start = start;
    fake_constant_count = count;
    fake_constant_words.fill(0);
    const size_t byte_count = static_cast<size_t>(count) * bytes_per_register;
    if (values && byte_count <= sizeof(fake_constant_words))
        std::memcpy(fake_constant_words.data(), values, byte_count);
}

HRESULT STDMETHODCALLTYPE fake_set_vertex_constant_f(IDirect3DDevice9 *, UINT start,
    const float *values, UINT count)
{
    capture_constants(1, start, values, count, 16);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE fake_set_vertex_constant_i(IDirect3DDevice9 *, UINT start,
    const int *values, UINT count)
{
    capture_constants(2, start, values, count, 16);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE fake_set_vertex_constant_b(IDirect3DDevice9 *, UINT start,
    const BOOL *values, UINT count)
{
    capture_constants(3, start, values, count, sizeof(BOOL));
    return S_OK;
}

HRESULT STDMETHODCALLTYPE fake_set_pixel_constant_f(IDirect3DDevice9 *, UINT start,
    const float *values, UINT count)
{
    capture_constants(4, start, values, count, 16);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE fake_set_pixel_constant_i(IDirect3DDevice9 *, UINT start,
    const int *values, UINT count)
{
    capture_constants(5, start, values, count, 16);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE fake_set_pixel_constant_b(IDirect3DDevice9 *, UINT start,
    const BOOL *values, UINT count)
{
    capture_constants(6, start, values, count, sizeof(BOOL));
    return S_OK;
}

HRESULT STDMETHODCALLTYPE fake_get_surface_desc(IDirect3DSurface9 *self,
                                                  D3DSURFACE_DESC *output)
{
    if (!output) return D3DERR_INVALIDCALL;
    *output = reinterpret_cast<FakeComObject *>(self)->description;
    return S_OK;
}

void *fake_factory(void *, uint32_t)
{
    return reinterpret_cast<IDirect3D9 *>(&fake_d3d);
}

void initialize_fake_com()
{
    fake_d3d_vtable.fill(nullptr);
    fake_device_vtable.fill(nullptr);
    fake_surface_vtable.fill(nullptr);
    fake_vertex_buffer_vtable.fill(nullptr);
    fake_cube_texture_vtable.fill(nullptr);
    fake_pixel_shader_vtable.fill(nullptr);
    fake_vertex_shader_vtable.fill(nullptr);
    for (auto *table : {&fake_d3d_vtable, &fake_surface_vtable}) {
        (*table)[0] = reinterpret_cast<void *>(&fake_query_interface);
        (*table)[1] = reinterpret_cast<void *>(&fake_add_ref);
        (*table)[2] = reinterpret_cast<void *>(&fake_release);
    }
    fake_cube_texture_vtable[0] = reinterpret_cast<void *>(&fake_query_interface);
    fake_cube_texture_vtable[1] = reinterpret_cast<void *>(&fake_add_ref);
    fake_cube_texture_vtable[2] = reinterpret_cast<void *>(&fake_release);
    fake_device_vtable[0] = reinterpret_cast<void *>(&fake_query_interface);
    fake_device_vtable[1] = reinterpret_cast<void *>(&fake_add_ref);
    fake_device_vtable[2] = reinterpret_cast<void *>(&fake_release);
    fake_device_vtable[3] = reinterpret_cast<void *>(&fake_test_cooperative_level);
    fake_device_vtable[17] = reinterpret_cast<void *>(&fake_present);
    fake_device_vtable[41] = reinterpret_cast<void *>(&fake_begin_scene);
    fake_device_vtable[42] = reinterpret_cast<void *>(&fake_end_scene);
    fake_device_vtable[81] = reinterpret_cast<void *>(&fake_draw_primitive);
    fake_device_vtable[82] = reinterpret_cast<void *>(&fake_draw_indexed_primitive);
    fake_pixel_shader_vtable[0] = reinterpret_cast<void *>(&fake_query_interface);
    fake_pixel_shader_vtable[1] = reinterpret_cast<void *>(&fake_add_ref);
    fake_pixel_shader_vtable[2] = reinterpret_cast<void *>(&fake_release);
    fake_vertex_shader_vtable[0] = reinterpret_cast<void *>(&fake_query_interface);
    fake_vertex_shader_vtable[1] = reinterpret_cast<void *>(&fake_add_ref);
    fake_vertex_shader_vtable[2] = reinterpret_cast<void *>(&fake_release);
    fake_vertex_buffer_vtable[0] = reinterpret_cast<void *>(&fake_query_interface);
    fake_vertex_buffer_vtable[1] = reinterpret_cast<void *>(&fake_add_ref);
    fake_vertex_buffer_vtable[2] = reinterpret_cast<void *>(&fake_release);
    fake_vertex_buffer_vtable[11] = reinterpret_cast<void *>(&fake_vertex_buffer_lock);
    fake_vertex_buffer_vtable[12] = reinterpret_cast<void *>(&fake_vertex_buffer_unlock);
    fake_vertex_buffer_vtable[13] = reinterpret_cast<void *>(&fake_vertex_buffer_get_desc);
    fake_d3d_vtable[16] = reinterpret_cast<void *>(&fake_create_device);
    fake_device_vtable[25] = reinterpret_cast<void *>(&fake_create_cube_texture);
    fake_device_vtable[26] = reinterpret_cast<void *>(&fake_create_vertex_buffer);
    fake_device_vtable[100] = reinterpret_cast<void *>(&fake_set_stream_source);
    fake_device_vtable[38] = reinterpret_cast<void *>(&fake_get_render_target);
    fake_device_vtable[37] = reinterpret_cast<void *>(&fake_set_render_target);
    fake_device_vtable[39] = reinterpret_cast<void *>(&fake_set_depth_surface);
    fake_device_vtable[40] = reinterpret_cast<void *>(&fake_get_depth_surface);
    fake_device_vtable[57] = reinterpret_cast<void *>(&fake_set_render_state);
    fake_device_vtable[7] = reinterpret_cast<void *>(&fake_get_device_caps);
    fake_device_vtable[65] = reinterpret_cast<void *>(&fake_set_texture);
    fake_device_vtable[67] = reinterpret_cast<void *>(&fake_set_texture_stage_state);
    fake_device_vtable[69] = reinterpret_cast<void *>(&fake_set_sampler_state);
    fake_device_vtable[92] = reinterpret_cast<void *>(&fake_set_vertex_shader);
    fake_device_vtable[91] = reinterpret_cast<void *>(&fake_create_vertex_shader);
    fake_device_vtable[94] = reinterpret_cast<void *>(&fake_set_vertex_constant_f);
    fake_device_vtable[96] = reinterpret_cast<void *>(&fake_set_vertex_constant_i);
    fake_device_vtable[98] = reinterpret_cast<void *>(&fake_set_vertex_constant_b);
    fake_device_vtable[107] = reinterpret_cast<void *>(&fake_set_pixel_shader);
    fake_device_vtable[106] = reinterpret_cast<void *>(&fake_create_pixel_shader);
    fake_device_vtable[109] = reinterpret_cast<void *>(&fake_set_pixel_constant_f);
    fake_device_vtable[111] = reinterpret_cast<void *>(&fake_set_pixel_constant_i);
    fake_device_vtable[113] = reinterpret_cast<void *>(&fake_set_pixel_constant_b);
    fake_surface_vtable[12] = reinterpret_cast<void *>(&fake_get_surface_desc);

    fake_d3d = {fake_d3d_vtable.data(), 1, {}};
    fake_device = {fake_device_vtable.data(), 0, {}};
    fake_vertex_buffer = {fake_vertex_buffer_vtable.data(), 0, {}};
    fake_vertex_buffer_size = static_cast<uint32_t>(fake_vertex_buffer_bytes.size());
    fake_vertex_buffer_bytes.fill(0);
    fake_vertex_buffer_locked = false;
    fake_bound_vertex_buffer = nullptr;
    fake_pixel_shader = {fake_pixel_shader_vtable.data(), 0, {}};
    fake_vertex_shader = {fake_vertex_shader_vtable.data(), 0, {}};
    fake_query_calls = 0;
    fake_render_state_type = UINT32_MAX;
    fake_render_state_value = UINT32_MAX;
    fake_texture_stage = UINT32_MAX;
    fake_texture_was_null = false;
    fake_texture_stage_type = UINT32_MAX;
    fake_texture_stage_value = UINT32_MAX;
    fake_sampler = UINT32_MAX;
    fake_sampler_type = UINT32_MAX;
    fake_sampler_value = UINT32_MAX;
    fake_null_shader_calls = 0;
    fake_pixel_shader_stream_ok = false;
    fake_pixel_shader_create_calls = 0;
    fake_vertex_shader_stream_ok = false;
    fake_vertex_shader_create_calls = 0;
    fake_constant_kind = 0;
    fake_constant_start = UINT32_MAX;
    fake_constant_count = UINT32_MAX;
    fake_constant_words.fill(0);
    fake_render_target = {fake_surface_vtable.data(), 0,
        {D3DFMT_A8R8G8B8, D3DRTYPE_SURFACE, D3DUSAGE_RENDERTARGET,
         D3DPOOL_DEFAULT, D3DMULTISAMPLE_NONE, 0, 640, 480}};
    fake_depth_surface = {fake_surface_vtable.data(), 0,
        {D3DFMT_D24S8, D3DRTYPE_SURFACE, D3DUSAGE_DEPTHSTENCIL,
         D3DPOOL_DEFAULT, D3DMULTISAMPLE_NONE, 0, 640, 480}};
    fake_cube_texture = {fake_cube_texture_vtable.data(), 0, {}};
    fake_cube_create_result = S_OK;
    fake_cube_create_calls = 0;
    fake_cube_create_args.fill(UINT32_MAX);
    fake_begin_scene_calls = 0;
    fake_end_scene_calls = 0;
    fake_draw_primitive_calls = 0;
    fake_draw_indexed_calls = 0;
    fake_set_depth_surface_calls = 0;
    fake_set_render_target_calls = 0;
    fake_present_calls = 0;
    fake_present_result = S_OK;
    fake_present_had_source = false;
    fake_present_had_destination = false;
    fake_present_hwnd = UINTPTR_MAX;
    fake_present_region_count = UINT32_MAX;
    fake_present_source = {};
    fake_present_destination = {};
    fake_present_first_dirty_rect = {};
}

bool run_fake_surface_smoke(ac1_d3d9_callbacks callbacks)
{
    initialize_fake_com();
    callbacks.factory = fake_factory;
    callbacks.guest_hwnd_to_host = fake_translate_window;
    if (!ac1_d3d9_configure(&callbacks, nullptr)) {
        std::fprintf(stderr, "fake bridge configure failed: %s\n", ac1_d3d9_last_error());
        return false;
    }
    const uint32_t d3d = ac1_d3d9_create9(0x20);
    if (!d3d) return false;

    const uint32_t pp_address = allocate_guest(nullptr, sizeof(GuestPresentParameters), 4);
    const uint32_t device_output = allocate_guest(nullptr, 4, 4);
    GuestPresentParameters pp{};
    pp.back_buffer_width = 640;
    pp.back_buffer_height = 480;
    pp.back_buffer_count = 1;
    pp.swap_effect = 1;
    pp.device_window = 0x1234u;
    pp.windowed = 1;
    pp.enable_auto_depth_stencil = 1;
    pp.auto_depth_stencil_format = D3DFMT_D24S8;
    std::memcpy(resolve_guest(nullptr, pp_address, sizeof(pp), AC1_D3D9_GUEST_WRITE), &pp,
                sizeof(pp));
    const uint32_t create_args[] = {d3d, 0, D3DDEVTYPE_HAL, 0x1234u,
                                    D3DCREATE_SOFTWARE_VERTEXPROCESSING,
                                    pp_address, device_output};
    uint32_t result = 0;
    if (ac1_d3d9_dispatch(0x7e001000u + 16 * 0x10u, create_args, 7, &result) != 1 ||
        FAILED(static_cast<HRESULT>(result))) {
        std::fprintf(stderr, "fake CreateDevice failed: 0x%08x %s\n", result,
                     ac1_d3d9_last_error());
        ac1_d3d9_shutdown();
        return false;
    }
    const uint32_t device = guest_word(device_output);
    const uint32_t cooperative_args[] = {device};
    if (ac1_d3d9_argument_count(object_slot(device, 3)) != 1 ||
        ac1_d3d9_dispatch(object_slot(device, 3), cooperative_args, 1, &result) != 1 ||
        static_cast<HRESULT>(result) != D3DERR_DEVICELOST) {
        std::fprintf(stderr, "fake TestCooperativeLevel HRESULT forwarding failed: 0x%08x %s\n",
                     result, ac1_d3d9_last_error());
        ac1_d3d9_shutdown();
        return false;
    }
    const uint32_t scene_args[] = {device};
    const uint32_t null_present_args[] = {device, 0u, 0u, 0u, 0u};
    if (ac1_d3d9_argument_count(object_slot(device, 17)) != 5 ||
        ac1_d3d9_argument_count(object_slot(device, 41)) != 1 ||
        ac1_d3d9_argument_count(object_slot(device, 42)) != 1 ||
        ac1_d3d9_dispatch(object_slot(device, 41), scene_args, 1, &result) != 1 ||
        FAILED(static_cast<HRESULT>(result)) ||
        ac1_d3d9_dispatch(object_slot(device, 42), scene_args, 1, &result) != 1 ||
        FAILED(static_cast<HRESULT>(result)) ||
        fake_begin_scene_calls != 1 || fake_end_scene_calls != 1) {
        std::fprintf(stderr, "fake BeginScene/EndScene ABI forwarding failed: %s\n",
                     ac1_d3d9_last_error());
        ac1_d3d9_shutdown();
        return false;
    }
    fake_present_result = D3DERR_DEVICELOST;
    if (ac1_d3d9_dispatch(object_slot(device, 17), null_present_args, 5,
                          &result) != 1 ||
        result != static_cast<uint32_t>(D3DERR_DEVICELOST) ||
        fake_present_calls != 1 || fake_present_had_source ||
        fake_present_had_destination || fake_present_hwnd != 0 ||
        fake_present_region_count != UINT32_MAX) {
        std::fprintf(stderr, "fake Present null arguments/HRESULT forwarding failed\n");
        ac1_d3d9_shutdown();
        return false;
    }
    const uint32_t fake_draw_args[] = {device, D3DPT_TRIANGLELIST, 0u, 1u};
    const uint32_t fake_draw_indexed_args[] = {
        device, D3DPT_TRIANGLELIST, 0u, 0u, 3u, 0u, 1u
    };
    if (ac1_d3d9_dispatch(object_slot(device, 81), fake_draw_args, 4,
                          &result) != 1 || FAILED(static_cast<HRESULT>(result)) ||
        ac1_d3d9_dispatch(object_slot(device, 82), fake_draw_indexed_args, 7,
                          &result) != 1 || FAILED(static_cast<HRESULT>(result)) ||
        fake_draw_primitive_calls != 1 || fake_draw_indexed_calls != 1) {
        std::fprintf(stderr, "fake DrawPrimitive/DrawIndexedPrimitive forwarding failed\n");
        ac1_d3d9_shutdown();
        return false;
    }
    static_assert(sizeof(RGNDATAHEADER) == 32,
                  "guest RGNDATAHEADER must match the PE32 ABI");
    const RECT fake_source_rect{1, 2, 30, 40};
    const RECT fake_destination_rect{5, 6, 70, 80};
    const uint32_t source_rect_address = allocate_guest(nullptr, sizeof(RECT), 4);
    const uint32_t destination_rect_address = allocate_guest(nullptr, sizeof(RECT), 4);
    const uint32_t dirty_region_address = allocate_guest(
        nullptr, sizeof(RGNDATAHEADER) + 2u * sizeof(RECT), 4);
    RGNDATAHEADER dirty_header{};
    dirty_header.dwSize = sizeof(RGNDATAHEADER);
    dirty_header.iType = RDH_RECTANGLES;
    dirty_header.nCount = 2;
    dirty_header.nRgnSize = 2u * sizeof(RECT);
    dirty_header.rcBound = RECT{1, 2, 30, 40};
    const RECT dirty_rects[] = {{1, 2, 10, 11}, {12, 13, 30, 40}};
    std::memcpy(resolve_guest(nullptr, source_rect_address, sizeof(RECT),
                              AC1_D3D9_GUEST_WRITE),
                &fake_source_rect, sizeof(RECT));
    std::memcpy(resolve_guest(nullptr, destination_rect_address, sizeof(RECT),
                              AC1_D3D9_GUEST_WRITE),
                &fake_destination_rect, sizeof(RECT));
    auto *dirty_guest = static_cast<uint8_t *>(resolve_guest(
        nullptr, dirty_region_address,
        sizeof(RGNDATAHEADER) + sizeof(dirty_rects), AC1_D3D9_GUEST_WRITE));
    std::memcpy(dirty_guest, &dirty_header, sizeof(dirty_header));
    std::memcpy(dirty_guest + sizeof(dirty_header), dirty_rects,
                sizeof(dirty_rects));
    const uint32_t mapped_present_args[] = {
        device, source_rect_address, destination_rect_address, 0x1234u,
        dirty_region_address
    };
    fake_present_result = S_OK;
    if (ac1_d3d9_dispatch(object_slot(device, 17), mapped_present_args, 5,
                          &result) != 1 || FAILED(static_cast<HRESULT>(result)) ||
        fake_present_calls != 2 || !fake_present_had_source ||
        !fake_present_had_destination || fake_present_hwnd != 1u ||
        fake_present_region_count != 2 ||
        std::memcmp(&fake_present_source, &fake_source_rect, sizeof(RECT)) != 0 ||
        std::memcmp(&fake_present_destination, &fake_destination_rect,
                    sizeof(RECT)) != 0 ||
        std::memcmp(&fake_present_first_dirty_rect, &dirty_rects[0],
                    sizeof(RECT)) != 0) {
        std::fprintf(stderr, "fake Present rectangle/region/HWND marshalling failed: %s\n",
                     ac1_d3d9_last_error());
        ac1_d3d9_shutdown();
        return false;
    }
    const uint32_t unmapped_present_rect_args[] = {
        device, 0xfffffff0u, 0u, 0u, 0u
    };
    const uint32_t unmapped_present_region_args[] = {
        device, 0u, 0u, 0u, 0xfffffff0u
    };
    const uint32_t unmapped_present_window_args[] = {
        device, 0u, 0u, 0x5678u, 0u
    };
    if (ac1_d3d9_dispatch(object_slot(device, 17), unmapped_present_rect_args,
                          5, &result) != 1 ||
        result != static_cast<uint32_t>(D3DERR_INVALIDCALL) ||
        ac1_d3d9_dispatch(object_slot(device, 17), unmapped_present_region_args,
                          5, &result) != 1 ||
        result != static_cast<uint32_t>(D3DERR_INVALIDCALL) ||
        ac1_d3d9_dispatch(object_slot(device, 17), unmapped_present_window_args,
                          5, &result) != 1 ||
        result != static_cast<uint32_t>(D3DERR_INVALIDCALL) ||
        fake_present_calls != 2) {
        std::fprintf(stderr, "fake Present accepted an unmapped pointer/HWND\n");
        ac1_d3d9_shutdown();
        return false;
    }
    dirty_header.iType = 0xdeadbeefu;
    std::memcpy(dirty_guest, &dirty_header, sizeof(dirty_header));
    if (ac1_d3d9_dispatch(object_slot(device, 17), mapped_present_args, 5,
                          &result) != -2 || fake_present_calls != 2) {
        std::fprintf(stderr, "Present did not reject an unsupported dirty-region type\n");
        ac1_d3d9_shutdown();
        return false;
    }
    const uint32_t cube_output = allocate_guest(nullptr, sizeof(uint32_t), 4);
    const uint32_t cube_create_args[] = {
        device, 4u, 1u, 0u, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
        cube_output, 0u
    };
    const uint32_t cube_unmapped_output_args[] = {
        device, 4u, 1u, 0u, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
        0xfffffffcu, 0u
    };
    const uint32_t cube_shared_handle_args[] = {
        device, 4u, 1u, 0u, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
        cube_output, 0x1234u
    };
    if (ac1_d3d9_argument_count(0x7e002190u) != 8 ||
        ac1_d3d9_dispatch(object_slot(device, 25), cube_unmapped_output_args, 8,
                          &result) != 1 ||
        result != static_cast<uint32_t>(D3DERR_INVALIDCALL) ||
        fake_cube_create_calls != 0 ||
        ac1_d3d9_dispatch(object_slot(device, 25), cube_shared_handle_args, 8,
                          &result) != -2 || fake_cube_create_calls != 0) {
        std::fprintf(stderr, "fake CreateCubeTexture output/shared-handle validation failed: %s\n",
                     ac1_d3d9_last_error());
        ac1_d3d9_shutdown();
        return false;
    }
    fake_cube_create_result = D3DERR_OUTOFVIDEOMEMORY;
    if (ac1_d3d9_dispatch(object_slot(device, 25), cube_create_args, 8,
                          &result) != 1 ||
        result != static_cast<uint32_t>(D3DERR_OUTOFVIDEOMEMORY) ||
        guest_word(cube_output) != 0 || fake_cube_create_calls != 1) {
        std::fprintf(stderr, "fake CreateCubeTexture HRESULT/output handling failed\n");
        ac1_d3d9_shutdown();
        return false;
    }
    fake_cube_create_result = S_OK;
    fail_next_guest_allocation = true;
    if (ac1_d3d9_dispatch(object_slot(device, 25), cube_create_args, 8,
                          &result) != 1 ||
        result != static_cast<uint32_t>(E_OUTOFMEMORY) ||
        guest_word(cube_output) != 0 || fake_cube_create_calls != 2 ||
        fake_cube_texture.references != 0) {
        std::fprintf(stderr, "fake CreateCubeTexture guest allocation failure cleanup failed: %s\n",
                     ac1_d3d9_last_error());
        ac1_d3d9_shutdown();
        return false;
    }
    if (ac1_d3d9_dispatch(object_slot(device, 25), cube_create_args, 8,
                          &result) != 1 || FAILED(static_cast<HRESULT>(result)) ||
        fake_cube_create_calls != 3 ||
        fake_cube_create_args != std::array<uint32_t, 6>{
            4u, 1u, 0u, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, 0u}) {
        std::fprintf(stderr, "fake CreateCubeTexture native ABI forwarding failed: %s\n",
                     ac1_d3d9_last_error());
        ac1_d3d9_shutdown();
        return false;
    }
    const uint32_t cube_texture = guest_word(cube_output);
    const uint32_t cube_release_args[] = {cube_texture};
    const uint32_t fake_cube_lock_args[] = {
        cube_texture, D3DCUBEMAP_FACE_POSITIVE_X, 0u, cube_output, 1u, 0u
    };
    if (!cube_texture || object_slot(cube_texture, 0) != 0x7e00b000u ||
        ac1_d3d9_argument_count(object_slot(cube_texture, 3)) != -2 ||
        ac1_d3d9_argument_count(object_slot(cube_texture, 19)) != 6 ||
        ac1_d3d9_argument_count(object_slot(cube_texture, 20)) != 3 ||
        ac1_d3d9_argument_count(0x7e00b160u) != -1 ||
        ac1_d3d9_dispatch(object_slot(cube_texture, 19), fake_cube_lock_args, 6,
                          &result) != -2 ||
        ac1_d3d9_dispatch(object_slot(cube_texture, 2), cube_release_args, 1,
                          &result) != 1 || result != 0 ||
        fake_cube_texture.references != 0) {
        std::fprintf(stderr, "fake cube-texture proxy/lifetime validation failed: %s\n",
                     ac1_d3d9_last_error());
        ac1_d3d9_shutdown();
        return false;
    }
    const uint32_t render_state_args[] = {device, 7u, 1u};
    if (ac1_d3d9_argument_count(object_slot(device, 57)) != 3 ||
        ac1_d3d9_dispatch(object_slot(device, 57), render_state_args, 3, &result) != 1 ||
        FAILED(static_cast<HRESULT>(result)) || fake_render_state_type != 7u ||
        fake_render_state_value != 1u) {
        std::fprintf(stderr, "fake SetRenderState ABI forwarding failed: 0x%08x %s\n",
                     result, ac1_d3d9_last_error());
        ac1_d3d9_shutdown();
        return false;
    }
    const uint32_t texture_args[] = {device, 2u, 0u};
    const uint32_t texture_stage_args[] = {device, 2u, 1u, 3u};
    const uint32_t sampler_args[] = {device, 0u, 6u, 2u};
    const uint32_t vertex_shader_args[] = {device, 0u};
    const uint32_t pixel_shader_args[] = {device, 0u};
    if (ac1_d3d9_argument_count(object_slot(device, 65)) != 3 ||
        ac1_d3d9_argument_count(object_slot(device, 67)) != 4 ||
        ac1_d3d9_argument_count(object_slot(device, 69)) != 4 ||
        ac1_d3d9_argument_count(object_slot(device, 92)) != 2 ||
        ac1_d3d9_argument_count(object_slot(device, 107)) != 2 ||
        ac1_d3d9_dispatch(object_slot(device, 65), texture_args, 3, &result) != 1 ||
        FAILED(static_cast<HRESULT>(result)) || fake_texture_stage != 2u ||
        !fake_texture_was_null ||
        ac1_d3d9_dispatch(object_slot(device, 67), texture_stage_args, 4, &result) != 1 ||
        FAILED(static_cast<HRESULT>(result)) || fake_texture_stage != 2u ||
        fake_texture_stage_type != 1u || fake_texture_stage_value != 3u ||
        ac1_d3d9_dispatch(object_slot(device, 69), sampler_args, 4, &result) != 1 ||
        FAILED(static_cast<HRESULT>(result)) || fake_sampler != 0u ||
        fake_sampler_type != 6u || fake_sampler_value != 2u ||
        ac1_d3d9_dispatch(object_slot(device, 92), vertex_shader_args, 2, &result) != 1 ||
        FAILED(static_cast<HRESULT>(result)) ||
        ac1_d3d9_dispatch(object_slot(device, 107), pixel_shader_args, 2, &result) != 1 ||
        FAILED(static_cast<HRESULT>(result)) || fake_null_shader_calls != 2u) {
        std::fprintf(stderr, "fake D3D9 state/null-resource ABI forwarding failed: %s\n",
                     ac1_d3d9_last_error());
        ac1_d3d9_shutdown();
        return false;
    }
    std::array<uint32_t, 110> shader_words{};
    shader_words[0] = D3DPS_VERSION(3, 0);
    shader_words[1] = (51u << D3DSI_COMMENTSIZE_SHIFT) | D3DSIO_COMMENT;
    shader_words[2] = D3DSIO_END; /* End-like data inside the CTAB comment. */
    for (uint32_t instruction = 0; instruction < 14; ++instruction)
        shader_words[53 + instruction * 4] = (3u << 24) | 1u;
    shader_words[54] = D3DSIO_END; /* End-like immediate inside an instruction. */
    shader_words[109] = D3DSIO_END;
    const uint32_t shader_code = allocate_guest(nullptr, sizeof(shader_words), 4);
    const uint32_t shader_output = allocate_guest(nullptr, sizeof(uint32_t), 4);
    std::memcpy(resolve_guest(nullptr, shader_code, sizeof(shader_words), AC1_D3D9_GUEST_WRITE),
                shader_words.data(), sizeof(shader_words));
    const uint32_t create_pixel_shader_args[] = {device, shader_code, shader_output};
    if (ac1_d3d9_argument_count(object_slot(device, 106)) != 3 ||
        ac1_d3d9_dispatch(object_slot(device, 106), create_pixel_shader_args, 3,
                          &result) != 1 || FAILED(static_cast<HRESULT>(result)) ||
        !fake_pixel_shader_stream_ok || fake_pixel_shader_create_calls != 1) {
        std::fprintf(stderr, "fake CreatePixelShader token-boundary/proxy setup failed: %s\n",
                     ac1_d3d9_last_error());
        ac1_d3d9_shutdown();
        return false;
    }
    const uint32_t guest_shader = guest_word(shader_output);
    const uint32_t bind_shader_args[] = {device, guest_shader};
    const uint32_t release_shader_args[] = {guest_shader};
    if (!guest_shader ||
        ac1_d3d9_dispatch(object_slot(device, 107), bind_shader_args, 2, &result) != 1 ||
        FAILED(static_cast<HRESULT>(result)) ||
        ac1_d3d9_dispatch(object_slot(guest_shader, 2), release_shader_args, 1,
                          &result) != 1 || result != 0 || fake_pixel_shader.references != 0) {
        std::fprintf(stderr, "fake pixel-shader bind/lifetime failed: %s\n",
                     ac1_d3d9_last_error());
        ac1_d3d9_shutdown();
        return false;
    }
    uint32_t bad_shader_output = 0xdeadbeefu;
    const uint32_t bad_shader_output_address = allocate_guest(
        nullptr, sizeof(bad_shader_output), 4);
    const uint32_t shader_version = D3DPS_VERSION(3, 0);
    std::memcpy(resolve_guest(nullptr, kGuestLimit - sizeof(shader_version),
                              sizeof(shader_version), AC1_D3D9_GUEST_WRITE),
                &shader_version, sizeof(shader_version));
    std::memcpy(resolve_guest(nullptr, bad_shader_output_address, sizeof(bad_shader_output),
                              AC1_D3D9_GUEST_WRITE),
                &bad_shader_output, sizeof(bad_shader_output));
    const uint32_t unterminated_shader_args[] = {
        device, kGuestLimit - sizeof(uint32_t), bad_shader_output_address
    };
    if (ac1_d3d9_dispatch(object_slot(device, 106), unterminated_shader_args, 3,
                          &result) != 1 ||
        result != static_cast<uint32_t>(D3DERR_INVALIDCALL) ||
        guest_word(bad_shader_output_address) != 0 || fake_pixel_shader_create_calls != 1) {
        std::fprintf(stderr, "unterminated pixel-shader token stream was not rejected safely\n");
        ac1_d3d9_shutdown();
        return false;
    }
    const uint32_t unsupported_shader_version = D3DPS_VERSION(2, 0);
    std::memcpy(resolve_guest(nullptr, shader_code, sizeof(unsupported_shader_version),
                              AC1_D3D9_GUEST_WRITE),
                &unsupported_shader_version, sizeof(unsupported_shader_version));
    const uint32_t unsupported_profile_args[] = {
        device, shader_code, bad_shader_output_address
    };
    result = 0xdeadbeefu;
    if (ac1_d3d9_dispatch(object_slot(device, 106), unsupported_profile_args, 3,
                          &result) != -2 || result != 0xdeadbeefu ||
        fake_pixel_shader_create_calls != 1) {
        std::fprintf(stderr, "unsupported shader profile was not explicitly classified\n");
        ac1_d3d9_shutdown();
        return false;
    }
    const std::array<uint32_t, 3> vertex_shader_words{{
        D3DVS_VERSION(3, 0), D3DSIO_NOP, D3DSIO_END
    }};
    const uint32_t vertex_shader_code = allocate_guest(
        nullptr, static_cast<uint32_t>(sizeof(vertex_shader_words)), 4);
    const uint32_t vertex_shader_output = allocate_guest(nullptr, sizeof(uint32_t), 4);
    std::memcpy(resolve_guest(nullptr, vertex_shader_code,
        static_cast<uint32_t>(sizeof(vertex_shader_words)), AC1_D3D9_GUEST_WRITE),
        vertex_shader_words.data(), sizeof(vertex_shader_words));
    const uint32_t create_vertex_shader_args[] = {
        device, vertex_shader_code, vertex_shader_output
    };
    if (ac1_d3d9_argument_count(object_slot(device, 91)) != 3 ||
        ac1_d3d9_dispatch(object_slot(device, 91), create_vertex_shader_args, 3,
                          &result) != 1 || FAILED(static_cast<HRESULT>(result)) ||
        !fake_vertex_shader_stream_ok || fake_vertex_shader_create_calls != 1) {
        std::fprintf(stderr, "fake CreateVertexShader token/proxy setup failed: %s\n",
                     ac1_d3d9_last_error());
        ac1_d3d9_shutdown();
        return false;
    }
    const uint32_t guest_vertex_shader = guest_word(vertex_shader_output);
    const uint32_t bind_vertex_shader_args[] = {device, guest_vertex_shader};
    const uint32_t release_vertex_shader_args[] = {guest_vertex_shader};
    if (!guest_vertex_shader ||
        ac1_d3d9_dispatch(object_slot(device, 92), bind_vertex_shader_args, 2,
                          &result) != 1 || FAILED(static_cast<HRESULT>(result)) ||
        ac1_d3d9_dispatch(object_slot(guest_vertex_shader, 2),
                          release_vertex_shader_args, 1, &result) != 1 ||
        result != 0 || fake_vertex_shader.references != 0) {
        std::fprintf(stderr, "fake vertex-shader bind/lifetime failed: %s\n",
                     ac1_d3d9_last_error());
        ac1_d3d9_shutdown();
        return false;
    }
    /* Releasing a still-locked guest buffer proxy must write its staging bytes
       back before native Unlock, even while the device retains a native ref. */
    const uint32_t cleanup_buffer_output = allocate_guest(nullptr, 4, 4);
    const uint32_t cleanup_create_buffer[] = {
        device, 64u, 0x208u, 0u, D3DPOOL_DEFAULT, cleanup_buffer_output, 0u
    };
    uint32_t cleanup_buffer = 0;
    const uint32_t cleanup_data[4] = {0x12345678u, 0x90abcdefu,
                                      0x0badf00du, 0x55aa33ccu};
    const uint32_t cleanup_lock_output = allocate_guest(nullptr, 4, 4);
    if (ac1_d3d9_dispatch(object_slot(device, 26), cleanup_create_buffer, 7,
                          &result) != 1 || FAILED(static_cast<HRESULT>(result)) ||
        !(cleanup_buffer = guest_word(cleanup_buffer_output))) {
        std::fprintf(stderr, "fake cleanup VB creation failed: %s\n",
                     ac1_d3d9_last_error());
        ac1_d3d9_shutdown();
        return false;
    }
    const uint32_t cleanup_lock[] = {
        cleanup_buffer, 0u, sizeof(cleanup_data), cleanup_lock_output,
        D3DLOCK_DISCARD
    };
    if (ac1_d3d9_dispatch(object_slot(cleanup_buffer, 11), cleanup_lock, 5,
                          &result) != 1 || FAILED(static_cast<HRESULT>(result))) {
        std::fprintf(stderr, "fake cleanup VB Lock failed: %s\n",
                     ac1_d3d9_last_error());
        ac1_d3d9_shutdown();
        return false;
    }
    const uint32_t cleanup_staging = guest_word(cleanup_lock_output);
    void *cleanup_guest_bytes = resolve_guest(nullptr, cleanup_staging,
        sizeof(cleanup_data), AC1_D3D9_GUEST_WRITE);
    if (!cleanup_guest_bytes) {
        ac1_d3d9_shutdown();
        return false;
    }
    std::memcpy(cleanup_guest_bytes, cleanup_data, sizeof(cleanup_data));
    const uint32_t cleanup_bind[] = {
        device, 0u, cleanup_buffer, 0u, sizeof(cleanup_data)
    };
    if (ac1_d3d9_dispatch(object_slot(device, 100), cleanup_bind, 5,
                          &result) != 1 || FAILED(static_cast<HRESULT>(result)) ||
        fake_vertex_buffer.references != 2 || !fake_bound_vertex_buffer) {
        std::fprintf(stderr, "fake cleanup VB bind failed: %s\n",
                     ac1_d3d9_last_error());
        ac1_d3d9_shutdown();
        return false;
    }
    const uint32_t cleanup_release[] = {cleanup_buffer};
    if (ac1_d3d9_dispatch(object_slot(cleanup_buffer, 2), cleanup_release, 1,
                          &result) != 1 || result != 0 ||
        fake_vertex_buffer.references != 1 || fake_vertex_buffer_locked ||
        std::memcmp(fake_vertex_buffer_bytes.data(), cleanup_data,
                    sizeof(cleanup_data)) != 0) {
        std::fprintf(stderr, "locked VB release did not write staged bytes before Unlock: %s\n",
                     ac1_d3d9_last_error());
        ac1_d3d9_shutdown();
        return false;
    }
    const uint32_t cleanup_unbind[] = {device, 0u, 0u, 0u, 0u};
    if (ac1_d3d9_dispatch(object_slot(device, 100), cleanup_unbind, 5,
                          &result) != 1 || FAILED(static_cast<HRESULT>(result)) ||
        fake_vertex_buffer.references != 0 || fake_bound_vertex_buffer) {
        std::fprintf(stderr, "fake cleanup VB native binding reference did not release\n");
        ac1_d3d9_shutdown();
        return false;
    }

    const uint32_t unregistered_texture_args[] = {device, 0u, 0x76543210u};
    const uint32_t unregistered_shader_args[] = {device, 0x76543210u};
    const uint32_t unregistered_declaration_args[] = {device, 0x76543210u};
    const uint32_t unregistered_stream_args[] = {device, 0u, 0x76543210u, 0u, 16u};
    const uint32_t unregistered_indices_args[] = {device, 0x76543210u};
    const uint32_t unsupported_query_args[] = {device, D3DQUERYTYPE_EVENT, 0u};
    if (ac1_d3d9_dispatch(object_slot(device, 65), unregistered_texture_args, 3,
                          &result) != -2 ||
        ac1_d3d9_dispatch(object_slot(device, 92), unregistered_shader_args, 2,
                          &result) != -2 ||
        ac1_d3d9_dispatch(object_slot(device, 87), unregistered_declaration_args, 2,
                          &result) != -2 ||
        ac1_d3d9_dispatch(object_slot(device, 100), unregistered_stream_args, 5,
                          &result) != -2 ||
        ac1_d3d9_dispatch(object_slot(device, 104), unregistered_indices_args, 2,
                          &result) != -2 ||
        ac1_d3d9_dispatch(object_slot(device, 118), unsupported_query_args, 3,
                          &result) != -2) {
        std::fprintf(stderr, "non-null unregistered D3D resource was not rejected\n");
        ac1_d3d9_shutdown();
        return false;
    }

    const std::array<float, 4> floats{{1.0f, -2.0f, 3.5f, 4.0f}};
    const std::array<int, 4> integers{{-1, 2, -3, 4}};
    const std::array<BOOL, 2> booleans{{TRUE, FALSE}};
    const uint32_t float_address = allocate_guest(nullptr, sizeof(floats), 4);
    const uint32_t integer_address = allocate_guest(nullptr, sizeof(integers), 4);
    const uint32_t boolean_address = allocate_guest(nullptr, sizeof(booleans), 4);
    std::memcpy(resolve_guest(nullptr, float_address, sizeof(floats), AC1_D3D9_GUEST_WRITE),
                floats.data(), sizeof(floats));
    std::memcpy(resolve_guest(nullptr, integer_address, sizeof(integers), AC1_D3D9_GUEST_WRITE),
                integers.data(), sizeof(integers));
    std::memcpy(resolve_guest(nullptr, boolean_address, sizeof(booleans), AC1_D3D9_GUEST_WRITE),
                booleans.data(), sizeof(booleans));
    const uint32_t vertex_float_args[] = {device, 1u, float_address, 1u};
    const uint32_t vertex_int_args[] = {device, 2u, integer_address, 1u};
    const uint32_t vertex_bool_args[] = {device, 3u, boolean_address, 2u};
    const uint32_t pixel_float_args[] = {device, 4u, float_address, 1u};
    const uint32_t pixel_int_args[] = {device, 5u, integer_address, 1u};
    const uint32_t pixel_bool_args[] = {device, 6u, boolean_address, 2u};
    const uint32_t constant_methods[] = {94u, 96u, 98u, 109u, 111u, 113u};
    const uint32_t *constant_args[] = {vertex_float_args, vertex_int_args,
        vertex_bool_args, pixel_float_args, pixel_int_args, pixel_bool_args};
    const uint32_t expected_constant_kinds[] = {1u, 2u, 3u, 4u, 5u, 6u};
    const uint32_t expected_constant_counts[] = {1u, 1u, 2u, 1u, 1u, 2u};
    bool constants_passed = true;
    for (size_t i = 0; i < sizeof(constant_methods) / sizeof(constant_methods[0]); ++i) {
        if (ac1_d3d9_argument_count(object_slot(device, constant_methods[i])) != 4 ||
            ac1_d3d9_dispatch(object_slot(device, constant_methods[i]),
                              constant_args[i], 4, &result) != 1 ||
            FAILED(static_cast<HRESULT>(result)) ||
            fake_constant_kind != expected_constant_kinds[i] ||
            fake_constant_start != constant_args[i][1] ||
            fake_constant_count != expected_constant_counts[i]) {
            constants_passed = false;
            break;
        }
        if (i == 0 && std::memcmp(fake_constant_words.data(), floats.data(), sizeof(floats)))
            constants_passed = false;
        if (i == 1 && std::memcmp(fake_constant_words.data(), integers.data(), sizeof(integers)))
            constants_passed = false;
        if (i == 2 && std::memcmp(fake_constant_words.data(), booleans.data(), sizeof(booleans)))
            constants_passed = false;
        if (i == 3 && std::memcmp(fake_constant_words.data(), floats.data(), sizeof(floats)))
            constants_passed = false;
        if (i == 4 && std::memcmp(fake_constant_words.data(), integers.data(), sizeof(integers)))
            constants_passed = false;
        if (i == 5 && std::memcmp(fake_constant_words.data(), booleans.data(), sizeof(booleans)))
            constants_passed = false;
    }
    const uint32_t unmapped_constant_args[] = {device, 0u, kGuestLimit - 1u, 1u};
    const uint32_t overrun_constant_args[] = {device, 256u, float_address, 1u};
    if (!constants_passed ||
        ac1_d3d9_dispatch(object_slot(device, 94), unmapped_constant_args, 4,
                          &result) != 1 || result != static_cast<uint32_t>(D3DERR_INVALIDCALL) ||
        ac1_d3d9_dispatch(object_slot(device, 94), overrun_constant_args, 4,
                          &result) != 1 || result != static_cast<uint32_t>(D3DERR_INVALIDCALL)) {
        std::fprintf(stderr, "fake shader constant marshalling/bounds failed: %s\n",
                     ac1_d3d9_last_error());
        ac1_d3d9_shutdown();
        return false;
    }
    const uint32_t render_output = allocate_guest(nullptr, 4, 4);
    const uint32_t depth_output = allocate_guest(nullptr, 4, 4);
    uint32_t bad_output = 0xdeadbeefu;
    std::memcpy(resolve_guest(nullptr, render_output, sizeof(bad_output), AC1_D3D9_GUEST_WRITE),
                &bad_output, sizeof(bad_output));
    const uint32_t bad_render_args[] = {device, 1, render_output};
    if (ac1_d3d9_dispatch(object_slot(device, 38), bad_render_args, 3, &result) != 1 ||
        result != static_cast<uint32_t>(D3DERR_INVALIDCALL) || guest_word(render_output) != 0) {
        std::fprintf(stderr, "fake GetRenderTarget HRESULT/output preservation failed\n");
        ac1_d3d9_shutdown();
        return false;
    }
    const uint32_t render_args[] = {device, 0, render_output};
    const uint32_t depth_args[] = {device, depth_output};
    if (ac1_d3d9_dispatch(object_slot(device, 38), render_args, 3, &result) != 1 ||
        FAILED(static_cast<HRESULT>(result)) ||
        ac1_d3d9_dispatch(object_slot(device, 40), depth_args, 2, &result) != 1 ||
        FAILED(static_cast<HRESULT>(result))) {
        std::fprintf(stderr, "fake surface getter failed: 0x%08x %s\n", result,
                     ac1_d3d9_last_error());
        ac1_d3d9_shutdown();
        return false;
    }
    const uint32_t bind_depth_args[] = {device, guest_word(depth_output)};
    const uint32_t unbind_depth_args[] = {device, 0u};
    const uint32_t bad_depth_args[] = {device, 0x76543210u};
    if (ac1_d3d9_argument_count(object_slot(device, 39)) != 2 ||
        ac1_d3d9_dispatch(object_slot(device, 39), bind_depth_args, 2, &result) != 1 ||
        FAILED(static_cast<HRESULT>(result)) ||
        ac1_d3d9_dispatch(object_slot(device, 39), unbind_depth_args, 2, &result) != 1 ||
        FAILED(static_cast<HRESULT>(result)) || !fake_depth_surface_was_null ||
        fake_set_depth_surface_calls != 2 ||
        ac1_d3d9_dispatch(object_slot(device, 39), bad_depth_args, 2, &result) != -2) {
        std::fprintf(stderr, "fake SetDepthStencilSurface typed/null forwarding failed: %s\n",
                     ac1_d3d9_last_error());
        ac1_d3d9_shutdown();
        return false;
    }
    const uint32_t bind_render_target_args[] = {device, 0u, guest_word(render_output)};
    const uint32_t clear_additional_target_args[] = {device, 1u, 0u};
    const uint32_t bad_render_target_args[] = {device, 1u, 0x76543210u};
    if (ac1_d3d9_argument_count(object_slot(device, 37)) != 3 ||
        ac1_d3d9_dispatch(object_slot(device, 37), bind_render_target_args, 3,
                          &result) != 1 || FAILED(static_cast<HRESULT>(result)) ||
        ac1_d3d9_dispatch(object_slot(device, 37), clear_additional_target_args, 3,
                          &result) != 1 || FAILED(static_cast<HRESULT>(result)) ||
        fake_set_render_target_calls != 2 || fake_render_target_index != 1 ||
        !fake_render_target_was_null ||
        ac1_d3d9_dispatch(object_slot(device, 37), bad_render_target_args, 3,
                          &result) != -2) {
        std::fprintf(stderr, "fake SetRenderTarget typed/null forwarding failed: %s\n",
                     ac1_d3d9_last_error());
        ac1_d3d9_shutdown();
        return false;
    }

    const uint32_t surface = guest_word(render_output);
    const uint32_t desc_address = allocate_guest(nullptr, sizeof(D3DSURFACE_DESC), 4);
    const uint32_t desc_args[] = {surface, desc_address};
    if (ac1_d3d9_dispatch(object_slot(surface, 12), desc_args, 2, &result) != 1 ||
        FAILED(static_cast<HRESULT>(result)) ||
        fake_render_target.references != 1) {
        std::fprintf(stderr, "fake surface GetDesc/ref failed: 0x%08x %s\n", result,
                     ac1_d3d9_last_error());
        ac1_d3d9_shutdown();
        return false;
    }
    uint32_t description_type = 0, description_width = 0, description_height = 0;
    std::memcpy(&description_type,
                resolve_guest(nullptr, desc_address + 4, sizeof(uint32_t), AC1_D3D9_GUEST_READ),
                sizeof(uint32_t));
    std::memcpy(&description_width,
                resolve_guest(nullptr, desc_address + 24, sizeof(uint32_t), AC1_D3D9_GUEST_READ),
                sizeof(uint32_t));
    std::memcpy(&description_height,
                resolve_guest(nullptr, desc_address + 28, sizeof(uint32_t), AC1_D3D9_GUEST_READ),
                sizeof(uint32_t));
    if (description_type != D3DRTYPE_SURFACE || description_width != 640 ||
        description_height != 480) {
        std::fprintf(stderr, "fake surface GetDesc marshaling mismatch (%u, %u x %u)\n",
                     description_type, description_width, description_height);
        ac1_d3d9_shutdown();
        return false;
    }

    const uint8_t iid_unimplemented[16] = {
        0x44, 0x33, 0x22, 0x11, 0x66, 0x55, 0x88, 0x77,
        0, 1, 2, 3, 4, 5, 6, 7
    };
    const uint32_t unsupported_iid_address = allocate_guest(
        nullptr, sizeof(iid_unimplemented), 4);
    const uint32_t unsupported_output = allocate_guest(nullptr, 4, 4);
    const uint32_t output_sentinel = 0xc001d00du;
    std::memcpy(resolve_guest(nullptr, unsupported_iid_address, sizeof(iid_unimplemented),
                              AC1_D3D9_GUEST_WRITE),
                iid_unimplemented, sizeof(iid_unimplemented));
    std::memcpy(resolve_guest(nullptr, unsupported_output, sizeof(output_sentinel),
                              AC1_D3D9_GUEST_WRITE),
                &output_sentinel, sizeof(output_sentinel));
    const uint32_t unsupported_args[] = {
        surface, unsupported_iid_address, unsupported_output
    };
    const uint32_t query_calls_before = fake_query_calls;
    result = 0xdeadbeefu;
    if (ac1_d3d9_dispatch(object_slot(surface, 0), unsupported_args, 3, &result) != -2 ||
        result != 0xdeadbeefu || guest_word(unsupported_output) != output_sentinel ||
        fake_query_calls != query_calls_before ||
        std::strstr(ac1_d3d9_last_error(), "11223344-5566-7788") == nullptr ||
        std::strstr(ac1_d3d9_last_error(), "native QueryInterface was not called") == nullptr) {
        std::fprintf(stderr, "unsupported IID was mistaken for a native E_NOINTERFACE: %s\n",
                     ac1_d3d9_last_error());
        ac1_d3d9_shutdown();
        return false;
    }

    const uint8_t iid_unknown[16] = {
        0, 0, 0, 0, 0, 0, 0, 0, 0xc0, 0, 0, 0, 0, 0, 0, 0x46
    };
    const uint32_t iid_address = allocate_guest(nullptr, sizeof(iid_unknown), 4);
    const uint32_t queried_output = allocate_guest(nullptr, 4, 4);
    std::memcpy(resolve_guest(nullptr, iid_address, sizeof(iid_unknown), AC1_D3D9_GUEST_WRITE),
                iid_unknown, sizeof(iid_unknown));
    const uint32_t query_args[] = {surface, iid_address, queried_output};
    if (ac1_d3d9_dispatch(object_slot(surface, 0), query_args, 3, &result) != 1 ||
        FAILED(static_cast<HRESULT>(result)) || guest_word(queried_output) != surface ||
        fake_render_target.references != 2) {
        std::fprintf(stderr, "fake IUnknown QueryInterface failed: %s\n",
                     ac1_d3d9_last_error());
        ac1_d3d9_shutdown();
        return false;
    }
    const uint32_t surface_this[] = {surface};
    uint32_t references = 0;
    if (ac1_d3d9_dispatch(object_slot(surface, 1), surface_this, 1, &references) != 1 ||
        references != 3 || fake_render_target.references != 3 ||
        ac1_d3d9_dispatch(object_slot(surface, 2), surface_this, 1, &references) != 1 ||
        references != 2 || fake_render_target.references != 2 ||
        ac1_d3d9_dispatch(object_slot(surface, 2), surface_this, 1, &references) != 1 ||
        references != 1 || fake_render_target.references != 1 ||
        ac1_d3d9_dispatch(object_slot(surface, 2), surface_this, 1, &references) != 1 ||
        references != 0 || fake_render_target.references != 0) {
        std::fprintf(stderr, "fake surface native lifetime balance failed: %s\n",
                     ac1_d3d9_last_error());
        ac1_d3d9_shutdown();
        return false;
    }

    const uint32_t depth = guest_word(depth_output);
    const uint32_t depth_this[] = {depth};
    if (ac1_d3d9_dispatch(object_slot(depth, 2), depth_this, 1, &references) != 1 ||
        references != 0 || fake_depth_surface.references != 0) {
        std::fprintf(stderr, "fake depth surface release failed: %s\n",
                     ac1_d3d9_last_error());
        ac1_d3d9_shutdown();
        return false;
    }
    const uint32_t device_this[] = {device};
    const uint32_t d3d_this[] = {d3d};
    if (ac1_d3d9_dispatch(object_slot(device, 2), device_this, 1, &references) != 1 ||
        references != 0 || fake_device.references != 0 ||
        ac1_d3d9_dispatch(object_slot(d3d, 2), d3d_this, 1, &references) != 1 ||
        references != 0 || fake_d3d.references != 0) {
        std::fprintf(stderr, "fake device/factory release failed: %s\n",
                     ac1_d3d9_last_error());
        ac1_d3d9_shutdown();
        return false;
    }
    ac1_d3d9_shutdown();
    std::printf("fake D3D9 state/surface ABI smoke passed (state setters, shader constants, surface getters and COM refs)\n");
    return true;
}

void *resolve_guest(void *, uint32_t address, uint32_t size, uint32_t)
{
    if (address < kGuestBase || address > kGuestLimit ||
        size > kGuestLimit - address) return nullptr;
    return guest_memory.data() + (address - kGuestBase);
}

uint32_t allocate_guest(void *, uint32_t size, uint32_t alignment)
{
    if (fail_next_guest_allocation) {
        fail_next_guest_allocation = false;
        return 0;
    }
    const uint32_t mask = alignment - 1;
    next_guest_address = (next_guest_address + mask) & ~mask;
    if (!size || next_guest_address > kGuestLimit || size > kGuestLimit - next_guest_address)
        return 0;
    uint32_t result = next_guest_address;
    next_guest_address += size;
    live_guest_allocations.insert(result);
    return result;
}

void free_guest(void *, uint32_t address)
{
    freed_guest_allocations.push_back(address);
    live_guest_allocations.erase(address);
}

bool guest_allocation_live(uint32_t address)
{
    return live_guest_allocations.find(address) != live_guest_allocations.end();
}

uintptr_t translate_window(void *, uint32_t guest_hwnd)
{
    return guest_hwnd == 0x1234u
        ? reinterpret_cast<uintptr_t>(hidden_window) : 0;
}

uint32_t guest_word(uint32_t address)
{
    uint32_t value = 0;
    std::memcpy(&value, resolve_guest(nullptr, address, sizeof(value), AC1_D3D9_GUEST_READ),
                sizeof(value));
    return value;
}

uint32_t object_slot(uint32_t object, uint32_t slot)
{
    const uint32_t table = guest_word(object);
    return guest_word(table + slot * sizeof(uint32_t));
}

void log_message(void *, const char *message)
{
    std::fprintf(stderr, "bridge: %s\n", message);
}

int run_provider_selection_case(const wchar_t *path, const char *expected_error)
{
    if (!path || !expected_error ||
        !SetEnvironmentVariableW(L"AC1_D3D9_LIBRARY", path)) {
        std::fprintf(stderr, "could not set AC1_D3D9_LIBRARY test value\n");
        return 20;
    }
    ac1_d3d9_callbacks callbacks{};
    callbacks.struct_size = sizeof(callbacks);
    callbacks.guest_resolve = resolve_guest;
    callbacks.guest_alloc = allocate_guest;
    callbacks.guest_free = free_guest;
    callbacks.guest_hwnd_to_host = translate_window;
    callbacks.log = log_message;
    if (!ac1_d3d9_configure(&callbacks, nullptr)) {
        std::fprintf(stderr, "provider test configure failed: %s\n",
                     ac1_d3d9_last_error());
        return 21;
    }
    const uint32_t object = ac1_d3d9_create9(0x20);
    const std::string error = ac1_d3d9_last_error();
    ac1_d3d9_shutdown();
    if (object || error.find(expected_error) == std::string::npos) {
        std::fprintf(stderr,
            "provider test did not fail explicitly as expected: object=0x%08x error=%s\n",
            object, error.c_str());
        return 22;
    }
    std::printf("explicit D3D9 provider rejection passed: %s\n", error.c_str());
    return 0;
}

int run_missing_provider_test()
{
    wchar_t temporary_directory[MAX_PATH]{};
    const DWORD length = GetTempPathW(MAX_PATH, temporary_directory);
    if (!length || length >= MAX_PATH) return 23;
    std::wstring path(temporary_directory, length);
    path += L"ac1_d3d9_missing_";
    path += std::to_wstring(GetCurrentProcessId());
    path += L".dll";
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        path += L"_absent";
        path += std::to_wstring(GetTickCount64());
    }
    return run_provider_selection_case(path.c_str(),
        "LoadLibraryExW failed for AC1_D3D9_LIBRARY");
}

int run_no_factory_export_test()
{
    wchar_t system_directory[MAX_PATH]{};
    const UINT length = GetSystemDirectoryW(system_directory, MAX_PATH);
    if (!length || length >= MAX_PATH - 14) return 24;
    std::wstring path(system_directory, length);
    path += L"\\kernel32.dll";
    return run_provider_selection_case(path.c_str(),
        "selected D3D9 provider has no Direct3DCreate9 export");
}
}

int main(int argc, char **argv)
{
    if (argc == 2 && !std::strcmp(argv[1], "--provider-relative"))
        return run_provider_selection_case(L"relative\\d3d9.dll",
            "AC1_D3D9_LIBRARY must be an absolute Windows DLL path");
    if (argc == 2 && !std::strcmp(argv[1], "--provider-missing"))
        return run_missing_provider_test();
    if (argc == 2 && !std::strcmp(argv[1], "--provider-no-export"))
        return run_no_factory_export_test();

    ac1_d3d9_callbacks callbacks{};
    callbacks.struct_size = sizeof(callbacks);
    callbacks.guest_resolve = resolve_guest;
    callbacks.guest_alloc = allocate_guest;
    callbacks.guest_free = free_guest;
    callbacks.guest_hwnd_to_host = translate_window;
    callbacks.log = log_message;
    if (argc == 2 && !std::strcmp(argv[1], "--fake-com"))
        return run_fake_surface_smoke(callbacks) ? 0 : 19;
    if (!ac1_d3d9_configure(&callbacks, nullptr)) {
        std::fprintf(stderr, "configure failed: %s\n", ac1_d3d9_last_error());
        return 1;
    }

    const uint32_t d3d9 = ac1_d3d9_create9(0x20);
    if (!d3d9) {
        std::fprintf(stderr, "Direct3DCreate9 failed: %s\n", ac1_d3d9_last_error());
        return 2;
    }
    if (!run_constant_table_smoke()) {
        std::fprintf(stderr, "D3DX constant-table bridge smoke failed\n");
        ac1_d3d9_shutdown();
        return 48;
    }
    uint32_t vtable = 0;
    std::memcpy(&vtable, resolve_guest(nullptr, d3d9, sizeof(vtable), AC1_D3D9_GUEST_READ),
                sizeof(vtable));
    uint32_t get_adapter_count = 0;
    std::memcpy(&get_adapter_count,
                resolve_guest(nullptr, vtable + 4 * 4, sizeof(get_adapter_count), AC1_D3D9_GUEST_READ),
                sizeof(get_adapter_count));
    if (ac1_d3d9_argument_count(get_adapter_count) != 1 ||
        ac1_d3d9_argument_count(0x7e002030u) != 1 ||
        ac1_d3d9_argument_count(0x7e002260u) != 3 ||
        ac1_d3d9_argument_count(0x7e002390u) != 3 ||
        ac1_d3d9_argument_count(0x7e002280u) != 2 ||
        ac1_d3d9_argument_count(0x7e002110u) != 5 ||
        ac1_d3d9_argument_count(0x7e002290u) != 1 ||
        ac1_d3d9_argument_count(0x7e0022a0u) != 1 ||
        ac1_d3d9_argument_count(0x7e003000u) != 3 ||
        ac1_d3d9_argument_count(0x7e0030c0u) != 2 ||
        ac1_d3d9_argument_count(0x7e0030b0u) != -2 ||
        ac1_d3d9_argument_count(0x7e0021a0u) != 7 ||
        ac1_d3d9_argument_count(0x7e0021b0u) != 7 ||
        ac1_d3d9_argument_count(0x7e004000u) != 3 ||
        ac1_d3d9_argument_count(0x7e005000u) != 3 ||
        ac1_d3d9_argument_count(0x7e006000u) != 3 ||
        ac1_d3d9_argument_count(0x7e007000u) != 3 ||
        ac1_d3d9_argument_count(0x7e002560u) != 3 ||
        ac1_d3d9_argument_count(0x7e002170u) != 9 ||
        ac1_d3d9_argument_count(0x7e002190u) != 8 ||
        ac1_d3d9_argument_count(0x7e00b000u) != 3 ||
        ac1_d3d9_argument_count(0x7e00b130u) != 6 ||
        ac1_d3d9_argument_count(0x7e00b140u) != 3 ||
        ac1_d3d9_argument_count(0x7e00b030u) != -2 ||
        ac1_d3d9_argument_count(0x7e00b160u) != -1 ||
        ac1_d3d9_argument_count(0x7e002510u) != 4 ||
        ac1_d3d9_argument_count(0x7e002520u) != 7 ||
        ac1_d3d9_argument_count(0x7e002570u) != 2 ||
        ac1_d3d9_argument_count(0x7e002640u) != 5 ||
        ac1_d3d9_argument_count(0x7e002680u) != 2 ||
        ac1_d3d9_argument_count(0x7e002760u) != 3 ||
        ac1_d3d9_argument_count(0x7e00c000u) != 3 ||
        ac1_d3d9_argument_count(0x7e00c060u) != 2 ||
        ac1_d3d9_argument_count(0x7e00c070u) != 4 ||
        ac1_d3d9_dispatch(0x7e0030b0u, nullptr, 0, nullptr) != -2 ||
        ac1_d3d9_dispatch(0x7e00c000u, nullptr, 0, nullptr) != -1) {
        std::fprintf(stderr, "COM method-arity classification failed\n");
        ac1_d3d9_shutdown();
        return 3;
    }

    const uint32_t this_args[] = {d3d9};
    uint32_t adapter_count = UINT32_MAX;
    if (ac1_d3d9_dispatch(get_adapter_count, this_args, 1, &adapter_count) != 1 ||
        adapter_count > 1024) {
        std::fprintf(stderr, "GetAdapterCount dispatch failed: %s\n", ac1_d3d9_last_error());
        ac1_d3d9_shutdown();
        return 4;
    }

    const uint32_t addref_token = 0x7e001000u + 1 * 0x10u;
    uint32_t references = 0;
    if (ac1_d3d9_dispatch(addref_token, this_args, 1, &references) != 1 || references != 2) {
        std::fprintf(stderr, "AddRef dispatch failed: %s\n", ac1_d3d9_last_error());
        ac1_d3d9_shutdown();
        return 5;
    }
    const uint32_t release_token = 0x7e001000u + 2 * 0x10u;
    uint32_t remaining = UINT32_MAX;
    if (ac1_d3d9_dispatch(release_token, this_args, 1, &remaining) != 1 || remaining != 1) {
        std::fprintf(stderr, "Release dispatch failed: %s\n", ac1_d3d9_last_error());
        ac1_d3d9_shutdown();
        return 6;
    }
    /* A hidden window plus windowed presentation avoids changing desktop mode. */
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = DefWindowProcW;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = L"AC1D3D9BridgeSmokeWindow";
    if (!RegisterClassW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        std::fprintf(stderr, "hidden-window class registration failed (%lu)\n", GetLastError());
        return 8;
    }
    hidden_window = CreateWindowExW(0, window_class.lpszClassName, L"AC1 D3D9 smoke",
                                    WS_OVERLAPPED, 0, 0, 640, 480, nullptr, nullptr,
                                    window_class.hInstance, nullptr);
    if (!hidden_window) {
        std::fprintf(stderr, "hidden-window creation failed (%lu)\n", GetLastError());
        return 9;
    }

    const uint32_t present_address = allocate_guest(nullptr, 56, 4);
    const uint32_t device_output = allocate_guest(nullptr, 4, 4);
    GuestPresentParameters present{};
    present.back_buffer_width = 640;
    present.back_buffer_height = 480;
    present.back_buffer_count = 1;
    present.swap_effect = 1; /* D3DSWAPEFFECT_DISCARD */
    present.device_window = 0x1234u;
    present.windowed = 1;
    present.enable_auto_depth_stencil = 1;
    present.auto_depth_stencil_format = 75; /* D3DFMT_D24S8 */
    present.presentation_interval = 0; /* D3DPRESENT_INTERVAL_DEFAULT */
    std::memcpy(resolve_guest(nullptr, present_address, sizeof(present), AC1_D3D9_GUEST_WRITE),
                &present, sizeof(present));
    const uint32_t create_args[] = {
        d3d9, 0, 1, 0x1234u, 0x20u, present_address, device_output
    };
    uint32_t create_result = 0;
    if (ac1_d3d9_dispatch(0x7e001000u + 16 * 0x10u, create_args, 7,
                          &create_result) != 1) {
        std::fprintf(stderr, "CreateDevice dispatch failed: %s\n", ac1_d3d9_last_error());
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 10;
    }
    const uint32_t device = guest_word(device_output);
    if (FAILED(static_cast<HRESULT>(create_result)) || !device) {
        /* Keep the native smoke non-invasive, then exercise the proxy with fake COM. */
        std::printf("windowed CreateDevice unavailable (HRESULT=0x%08x); using fake COM surface smoke\n",
                    create_result);
        ac1_d3d9_shutdown();
        const bool fake_passed = run_fake_surface_smoke(callbacks);
        DestroyWindow(hidden_window);
        return fake_passed ? 0 : 19;
    }

    const uint32_t frame_this[] = {device};
    const uint32_t null_present[] = {device, 0u, 0u, 0u, 0u};
    uint32_t frame_result = 0;
    if (ac1_d3d9_dispatch(object_slot(device, 41), frame_this, 1,
                          &frame_result) != 1 ||
        FAILED(static_cast<HRESULT>(frame_result)) ||
        ac1_d3d9_dispatch(object_slot(device, 42), frame_this, 1,
                          &frame_result) != 1 ||
        FAILED(static_cast<HRESULT>(frame_result)) ||
        ac1_d3d9_dispatch(object_slot(device, 17), null_present, 5,
                          &frame_result) != 1 ||
        FAILED(static_cast<HRESULT>(frame_result))) {
        std::fprintf(stderr, "native DXVK BeginScene/EndScene/Present failed: %s (HRESULT=0x%08x)\n",
                     ac1_d3d9_last_error(), frame_result);
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 73;
    }
    std::printf("native DXVK BeginScene/EndScene/Present passed without draw submission\n");

    const uint32_t cube_output = allocate_guest(nullptr, sizeof(uint32_t), 4);
    const uint32_t cube_create_args[] = {
        device, 4u, 1u, 0u, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
        cube_output, 0u
    };
    uint32_t cube_result = 0;
    if (ac1_d3d9_argument_count(object_slot(device, 25)) != 8 ||
        ac1_d3d9_dispatch(object_slot(device, 25), cube_create_args, 8,
                          &cube_result) != 1 ||
        FAILED(static_cast<HRESULT>(cube_result))) {
        std::fprintf(stderr, "native DXVK CreateCubeTexture failed: %s (HRESULT=0x%08x)\n",
                     ac1_d3d9_last_error(), cube_result);
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 58;
    }
    const uint32_t cube_texture = guest_word(cube_output);
    const uint32_t cube_lock = object_slot(cube_texture, 19);
    const uint32_t cube_unlock = object_slot(cube_texture, 20);
    const uint32_t cube_locked_output = allocate_guest(nullptr, sizeof(GuestLockedRect), 4);
    const uint32_t cube_bad_output_args[] = {
        cube_texture, D3DCUBEMAP_FACE_POSITIVE_X, 0u, 0xfffffffcu, 0u, 0u
    };
    const uint32_t cube_bad_face_args[] = {
        cube_texture, 6u, 0u, cube_locked_output, 0u, 0u
    };
    const uint32_t cube_bad_rect_args[] = {
        cube_texture, D3DCUBEMAP_FACE_POSITIVE_X, 0u, cube_locked_output, 1u, 0u
    };
    const uint32_t cube_bad_flags_args[] = {
        cube_texture, D3DCUBEMAP_FACE_POSITIVE_X, 0u, cube_locked_output, 0u,
        D3DLOCK_READONLY
    };
    if (!cube_texture || object_slot(cube_texture, 0) != 0x7e00b000u ||
        ac1_d3d9_argument_count(cube_lock) != 6 ||
        ac1_d3d9_argument_count(cube_unlock) != 3 ||
        ac1_d3d9_dispatch(cube_lock, cube_bad_output_args, 6, &cube_result) != 1 ||
        cube_result != static_cast<uint32_t>(D3DERR_INVALIDCALL) ||
        ac1_d3d9_dispatch(cube_lock, cube_bad_face_args, 6, &cube_result) != 1 ||
        cube_result != static_cast<uint32_t>(D3DERR_INVALIDCALL) ||
        ac1_d3d9_dispatch(cube_lock, cube_bad_rect_args, 6, &cube_result) != -2 ||
        ac1_d3d9_dispatch(cube_lock, cube_bad_flags_args, 6, &cube_result) != -2) {
        std::fprintf(stderr, "native cube LockRect ABI/profile validation failed: %s\n",
                     ac1_d3d9_last_error());
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 60;
    }

    std::array<GuestLockedRect, 6> face_locks{};
    for (uint32_t face = 0; face < face_locks.size(); ++face) {
        const uint32_t lock_args[] = {cube_texture, face, 0u,
                                      cube_locked_output, 0u, 0u};
        if (ac1_d3d9_dispatch(cube_lock, lock_args, 6, &cube_result) != 1 ||
            FAILED(static_cast<HRESULT>(cube_result))) {
            std::fprintf(stderr, "native cube face %u LockRect failed: %s (HRESULT=0x%08x)\n",
                         face, ac1_d3d9_last_error(), cube_result);
            DestroyWindow(hidden_window);
            ac1_d3d9_shutdown();
            return 61;
        }
        std::memcpy(&face_locks[face],
                    resolve_guest(nullptr, cube_locked_output,
                                  sizeof(GuestLockedRect), AC1_D3D9_GUEST_READ),
                    sizeof(GuestLockedRect));
        if (face_locks[face].pitch < 16 || !face_locks[face].bits ||
            face_locks[face].bits < kGuestBase ||
            static_cast<uint64_t>(face_locks[face].bits) +
                static_cast<uint64_t>(face_locks[face].pitch) * 4u > kGuestLimit) {
            std::fprintf(stderr, "native cube face %u returned invalid guest pitch/bits\n", face);
            DestroyWindow(hidden_window);
            ac1_d3d9_shutdown();
            return 62;
        }
        auto *pixels = static_cast<uint32_t *>(resolve_guest(
            nullptr, face_locks[face].bits,
            static_cast<uint32_t>(face_locks[face].pitch) * 4u,
            AC1_D3D9_GUEST_WRITE));
        if (!pixels) {
            std::fprintf(stderr, "native cube face %u staging pointer is not guest memory\n", face);
            DestroyWindow(hidden_window);
            ac1_d3d9_shutdown();
            return 63;
        }
        for (uint32_t y = 0; y < 4; ++y) {
            for (uint32_t x = 0; x < 4; ++x) {
                pixels[static_cast<size_t>(y) * face_locks[face].pitch / 4u + x] =
                    0xff000000u | ((face + 1u) << 16u) | (y << 8u) | x;
            }
        }
    }
    const uint32_t cube_duplicate_lock_args[] = {
        cube_texture, D3DCUBEMAP_FACE_POSITIVE_X, 0u, cube_locked_output, 0u, 0u
    };
    const uint32_t cube_unmatched_unlock_args[] = {
        cube_texture, D3DCUBEMAP_FACE_POSITIVE_X, 1u
    };
    if (ac1_d3d9_dispatch(cube_lock, cube_duplicate_lock_args, 6, &cube_result) != 1 ||
        cube_result != static_cast<uint32_t>(D3DERR_INVALIDCALL) ||
        ac1_d3d9_dispatch(cube_unlock, cube_unmatched_unlock_args, 3,
                          &cube_result) != 1 ||
        cube_result != static_cast<uint32_t>(D3DERR_INVALIDCALL)) {
        std::fprintf(stderr, "cube face/level lock identity validation failed: %s\n",
                     ac1_d3d9_last_error());
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 64;
    }
    for (uint32_t face = 0; face < face_locks.size(); ++face) {
        const uint32_t unlock_args[] = {cube_texture, face, 0u};
        if (ac1_d3d9_dispatch(cube_unlock, unlock_args, 3, &cube_result) != 1 ||
            FAILED(static_cast<HRESULT>(cube_result))) {
            std::fprintf(stderr, "native cube face %u UnlockRect failed: %s (HRESULT=0x%08x)\n",
                         face, ac1_d3d9_last_error(), cube_result);
            DestroyWindow(hidden_window);
            ac1_d3d9_shutdown();
            return 65;
        }
    }
    for (uint32_t face = 0; face < face_locks.size(); ++face) {
        const uint32_t lock_args[] = {cube_texture, face, 0u,
                                      cube_locked_output, 0u, 0u};
        if (ac1_d3d9_dispatch(cube_lock, lock_args, 6, &cube_result) != 1 ||
            FAILED(static_cast<HRESULT>(cube_result))) {
            std::fprintf(stderr, "native cube face %u readback LockRect failed\n", face);
            DestroyWindow(hidden_window);
            ac1_d3d9_shutdown();
            return 66;
        }
        GuestLockedRect readback{};
        std::memcpy(&readback,
                    resolve_guest(nullptr, cube_locked_output,
                                  sizeof(GuestLockedRect), AC1_D3D9_GUEST_READ),
                    sizeof(readback));
        auto *pixels = static_cast<const uint32_t *>(resolve_guest(
            nullptr, readback.bits, static_cast<uint32_t>(readback.pitch) * 4u,
            AC1_D3D9_GUEST_READ));
        bool face_matches = pixels != nullptr && readback.pitch == face_locks[face].pitch;
        for (uint32_t y = 0; y < 4 && face_matches; ++y) {
            for (uint32_t x = 0; x < 4; ++x) {
                const uint32_t expected = 0xff000000u |
                    ((face + 1u) << 16u) | (y << 8u) | x;
                if (pixels[static_cast<size_t>(y) * readback.pitch / 4u + x] != expected) {
                    face_matches = false;
                    break;
                }
            }
        }
        const uint32_t unlock_args[] = {cube_texture, face, 0u};
        if (!face_matches ||
            ac1_d3d9_dispatch(cube_unlock, unlock_args, 3, &cube_result) != 1 ||
            FAILED(static_cast<HRESULT>(cube_result))) {
            std::fprintf(stderr, "native cube face %u row-pitch readback mismatch\n", face);
            DestroyWindow(hidden_window);
            ac1_d3d9_shutdown();
            return 67;
        }
    }
    const uint32_t cube_outstanding_lock_args[] = {
        cube_texture, D3DCUBEMAP_FACE_POSITIVE_X, 0u, cube_locked_output, 0u, 0u
    };
    if (ac1_d3d9_dispatch(cube_lock, cube_outstanding_lock_args, 6,
                          &cube_result) != 1 ||
        FAILED(static_cast<HRESULT>(cube_result))) {
        std::fprintf(stderr, "cube final-release cleanup LockRect failed\n");
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 68;
    }
    GuestLockedRect release_locked{};
    std::memcpy(&release_locked,
                resolve_guest(nullptr, cube_locked_output,
                              sizeof(GuestLockedRect), AC1_D3D9_GUEST_READ),
                sizeof(release_locked));
    const uint32_t cube_release_args[] = {cube_texture};
    if (!cube_texture || object_slot(cube_texture, 0) != 0x7e00b000u ||
        ac1_d3d9_argument_count(object_slot(cube_texture, 3)) != -2 ||
        ac1_d3d9_dispatch(object_slot(cube_texture, 2), cube_release_args, 1,
                          &cube_result) != 1 || cube_result != 0) {
        std::fprintf(stderr, "native DXVK cube-texture proxy release failed: %s\n",
                     ac1_d3d9_last_error());
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 59;
    }
    if (std::find(freed_guest_allocations.begin(), freed_guest_allocations.end(),
                  release_locked.bits) == freed_guest_allocations.end()) {
        std::fprintf(stderr, "cube final Release did not free locked guest staging\n");
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 69;
    }

    const uint32_t shutdown_cube_output = allocate_guest(nullptr, 4, 4);
    const uint32_t shutdown_cube_create_args[] = {
        device, 4u, 1u, 0u, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
        shutdown_cube_output, 0u
    };
    if (ac1_d3d9_dispatch(object_slot(device, 25), shutdown_cube_create_args, 8,
                          &cube_result) != 1 ||
        FAILED(static_cast<HRESULT>(cube_result))) {
        std::fprintf(stderr, "shutdown-cleanup cube creation failed\n");
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 70;
    }
    const uint32_t shutdown_cube = guest_word(shutdown_cube_output);
    const uint32_t shutdown_cube_locked_output = allocate_guest(
        nullptr, sizeof(GuestLockedRect), 4);
    const uint32_t shutdown_cube_lock_args[] = {
        shutdown_cube, D3DCUBEMAP_FACE_NEGATIVE_Z, 0u,
        shutdown_cube_locked_output, 0u, 0u
    };
    if (ac1_d3d9_dispatch(object_slot(shutdown_cube, 19),
                          shutdown_cube_lock_args, 6, &cube_result) != 1 ||
        FAILED(static_cast<HRESULT>(cube_result))) {
        std::fprintf(stderr, "shutdown-cleanup cube LockRect failed\n");
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 71;
    }
    GuestLockedRect shutdown_cube_locked{};
    std::memcpy(&shutdown_cube_locked,
                resolve_guest(nullptr, shutdown_cube_locked_output,
                              sizeof(GuestLockedRect), AC1_D3D9_GUEST_READ),
                sizeof(shutdown_cube_locked));

    std::printf("native DXVK cube six-face LockRect round-trip plus Release cleanup passed\n");

    static const char pixel_source[] =
        "float4 main() : COLOR0 { return float4(1.0, 0.0, 0.0, 1.0); }";
    static const char vertex_source[] =
        "float4 main(float4 position : POSITION) : POSITION { return position; }";
    if (!run_native_shader_smoke(device, pixel_source, "ps_3_0", 106, 107, "Pixel") ||
        !run_native_shader_smoke(device, vertex_source, "vs_3_0", 91, 92, "Vertex")) {
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 46;
    }

    uint32_t getter_result = 0;
    const uint32_t width_address = allocate_guest(nullptr, 4, 4);
    const uint32_t height_address = allocate_guest(nullptr, 4, 4);
    const uint32_t mip_address = allocate_guest(nullptr, 4, 4);
    const uint32_t format_address = allocate_guest(nullptr, 4, 4);
    const uint32_t requested_width = 4, requested_height = 4, requested_mips = 1;
    const uint32_t requested_format = D3DFMT_A8R8G8B8;
    std::memcpy(resolve_guest(nullptr, width_address, 4, AC1_D3D9_GUEST_WRITE),
                &requested_width, 4);
    std::memcpy(resolve_guest(nullptr, height_address, 4, AC1_D3D9_GUEST_WRITE),
                &requested_height, 4);
    std::memcpy(resolve_guest(nullptr, mip_address, 4, AC1_D3D9_GUEST_WRITE),
                &requested_mips, 4);
    std::memcpy(resolve_guest(nullptr, format_address, 4, AC1_D3D9_GUEST_WRITE),
                &requested_format, 4);
    uint32_t d3dx_hresult = 0;
    if (ac1_d3d9_check_texture_requirements(
            device, width_address, height_address, mip_address, 0,
            format_address, D3DPOOL_MANAGED, &d3dx_hresult) != 1 ||
        FAILED(static_cast<HRESULT>(d3dx_hresult))) {
        std::fprintf(stderr, "D3DXCheckTextureRequirements failed: %s (HRESULT=0x%08x)\n",
                     ac1_d3d9_last_error(), d3dx_hresult);
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 38;
    }
    std::printf("native System32 D3DXCheckTextureRequirements hr=0x%08x corrected=%ux%u mip=%u format=%u\n",
                d3dx_hresult, guest_word(width_address), guest_word(height_address),
                guest_word(mip_address), guest_word(format_address));
    if (ac1_d3d9_check_texture_requirements(
            device, 0xfffffff0u, height_address, mip_address, 0,
            format_address, D3DPOOL_MANAGED, &d3dx_hresult) != -1) {
        std::fprintf(stderr, "D3DX helper accepted an unmapped guest in/out pointer\n");
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 39;
    }
    const uint32_t texture_output = allocate_guest(nullptr, 4, 4);
    const uint32_t create_texture_args[] = {
        device, guest_word(width_address), guest_word(height_address),
        guest_word(mip_address), 0u, guest_word(format_address),
        D3DPOOL_MANAGED, texture_output, 0u
    };
    if (ac1_d3d9_dispatch(object_slot(device, 23), create_texture_args, 9,
                          &getter_result) != 1 ||
        FAILED(static_cast<HRESULT>(getter_result)) || !guest_word(texture_output)) {
        std::fprintf(stderr, "native CreateTexture failed: %s (HRESULT=0x%08x)\n",
                     ac1_d3d9_last_error(), getter_result);
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 40;
    }
    std::printf("native CreateTexture hr=0x%08x proxy=0x%08x\n",
                getter_result, guest_word(texture_output));
    const uint32_t texture = guest_word(texture_output);
    if (ac1_d3d9_argument_count(object_slot(texture, 2)) != 1) {
        std::fprintf(stderr, "created D3D9 texture did not receive a texture proxy kind\n");
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 41;
    }
    const uint32_t texture_iid_bytes[4] = {
        0x85c31227u, 0x4f003de5u, 0x1af13a9bu, 0xb5188cc3u
    };
    const uint32_t texture_iid_address = allocate_guest(nullptr, 16, 4);
    const uint32_t texture_query_output = allocate_guest(nullptr, 4, 4);
    std::memcpy(resolve_guest(nullptr, texture_iid_address, 16, AC1_D3D9_GUEST_WRITE),
                texture_iid_bytes, 16);
    const uint32_t texture_query_args[] = {
        texture, texture_iid_address, texture_query_output
    };
    if (ac1_d3d9_dispatch(object_slot(texture, 0), texture_query_args, 3,
                          &getter_result) != 1 ||
        FAILED(static_cast<HRESULT>(getter_result)) ||
        guest_word(texture_query_output) != texture) {
        std::fprintf(stderr, "texture QueryInterface identity failed: %s\n",
                     ac1_d3d9_last_error());
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 42;
    }
    const uint32_t texture_this[] = {texture};
    if (ac1_d3d9_dispatch(object_slot(texture, 2), texture_this, 1,
                          &references) != 1 || references != 1) {
        std::fprintf(stderr, "texture QueryInterface reference balance failed\n");
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 43;
    }
    const uint32_t locked_rect_output = allocate_guest(nullptr, 8, 4);
    const uint32_t lock_texture_args[] = {texture, 0u, locked_rect_output, 0u, 0u};
    if (ac1_d3d9_argument_count(object_slot(texture, 19)) != 5 ||
        ac1_d3d9_argument_count(object_slot(texture, 20)) != 2 ||
        ac1_d3d9_dispatch(object_slot(texture, 19), lock_texture_args, 5,
                          &getter_result) != 1 ||
        FAILED(static_cast<HRESULT>(getter_result))) {
        std::fprintf(stderr, "native texture LockRect failed: %s (HRESULT=0x%08x)\n",
                     ac1_d3d9_last_error(), getter_result);
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 46;
    }
    int32_t locked_pitch = 0;
    uint32_t locked_bits = 0;
    std::memcpy(&locked_pitch,
                resolve_guest(nullptr, locked_rect_output, 4, AC1_D3D9_GUEST_READ), 4);
    std::memcpy(&locked_bits,
                resolve_guest(nullptr, locked_rect_output + 4, 4, AC1_D3D9_GUEST_READ), 4);
    std::array<uint8_t, 64> expected_texture_bytes{};
    for (uint32_t i = 0; i < expected_texture_bytes.size(); ++i)
        expected_texture_bytes[i] = static_cast<uint8_t>(i * 3u + 7u);
    if (locked_pitch < 16 || !locked_bits) {
        std::fprintf(stderr, "texture LockRect returned invalid guest pitch/bits\n");
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 47;
    }
    auto *locked_staging = static_cast<uint8_t *>(resolve_guest(nullptr, locked_bits,
        static_cast<uint32_t>(locked_pitch) * 4u, AC1_D3D9_GUEST_WRITE));
    if (!locked_staging) {
        std::fprintf(stderr, "texture LockRect staging span is unmapped\n");
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 48;
    }
    for (uint32_t row = 0; row < 4; ++row)
        std::memcpy(locked_staging + static_cast<size_t>(row) * locked_pitch,
                    expected_texture_bytes.data() + row * 16u, 16u);
    const uint32_t unlock_texture_args[] = {texture, 0u};
    if (ac1_d3d9_dispatch(object_slot(texture, 20), unlock_texture_args, 2,
                          &getter_result) != 1 ||
        FAILED(static_cast<HRESULT>(getter_result)) ||
        ac1_d3d9_dispatch(object_slot(texture, 19), lock_texture_args, 5,
                          &getter_result) != 1 ||
        FAILED(static_cast<HRESULT>(getter_result))) {
        std::fprintf(stderr, "texture UnlockRect/relock failed: %s (HRESULT=0x%08x)\n",
                     ac1_d3d9_last_error(), getter_result);
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 49;
    }
    std::memcpy(&locked_pitch,
                resolve_guest(nullptr, locked_rect_output, 4, AC1_D3D9_GUEST_READ), 4);
    std::memcpy(&locked_bits,
                resolve_guest(nullptr, locked_rect_output + 4, 4, AC1_D3D9_GUEST_READ), 4);
    locked_staging = static_cast<uint8_t *>(resolve_guest(nullptr, locked_bits,
        static_cast<uint32_t>(locked_pitch) * 4u, AC1_D3D9_GUEST_READ));
    bool texture_roundtrip_ok = locked_staging != nullptr;
    for (uint32_t row = 0; texture_roundtrip_ok && row < 4; ++row) {
        if (std::memcmp(locked_staging + static_cast<size_t>(row) * locked_pitch,
                        expected_texture_bytes.data() + row * 16u, 16u) != 0)
            texture_roundtrip_ok = false;
    }
    if (!texture_roundtrip_ok ||
        ac1_d3d9_dispatch(object_slot(texture, 20), unlock_texture_args, 2,
                          &getter_result) != 1 ||
        FAILED(static_cast<HRESULT>(getter_result))) {
        std::fprintf(stderr, "texture LockRect staging readback failed: %s\n",
                     ac1_d3d9_last_error());
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 50;
    }
    std::printf("native texture LockRect round-trip passed; pitch=%d rows=4 bytes=64\n",
                locked_pitch);
    const RECT unsupported_texture_rect{0, 0, 4, 4};
    const uint32_t unsupported_rect_address = allocate_guest(
        nullptr, sizeof(unsupported_texture_rect), 4);
    std::memcpy(resolve_guest(nullptr, unsupported_rect_address,
                              sizeof(unsupported_texture_rect), AC1_D3D9_GUEST_WRITE),
                &unsupported_texture_rect, sizeof(unsupported_texture_rect));
    const uint32_t unsupported_rect_lock_args[] = {
        texture, 0u, locked_rect_output, unsupported_rect_address, 0u
    };
    const uint32_t unsupported_flags_lock_args[] = {
        texture, 0u, locked_rect_output, 0u, D3DLOCK_READONLY
    };
    if (ac1_d3d9_dispatch(object_slot(texture, 19), unsupported_rect_lock_args, 5,
                          &getter_result) != -2 ||
        ac1_d3d9_dispatch(object_slot(texture, 19), unsupported_flags_lock_args, 5,
                          &getter_result) != -2) {
        std::fprintf(stderr, "unsupported texture LockRect profile was not trapped\n");
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 51;
    }
    const uint32_t unsupported_texture_shared_args[] = {
        device, guest_word(width_address), guest_word(height_address),
        guest_word(mip_address), 0u, guest_word(format_address),
        D3DPOOL_MANAGED, texture_output, 1u
    };
    if (ac1_d3d9_dispatch(object_slot(device, 23),
                          unsupported_texture_shared_args, 9, &getter_result) != -2) {
        std::fprintf(stderr, "non-null shared texture handle was not trapped explicitly\n");
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 44;
    }
    const uint32_t vertex_buffer_output = allocate_guest(nullptr, 4, 4);
    const uint32_t index_buffer_output = allocate_guest(nullptr, 4, 4);
    const uint32_t create_vertex_buffer_args[] = {
        device, 0x10000u, 0x208u, 0u, 0u, vertex_buffer_output, 0u
    };
    const uint32_t create_index_buffer_args[] = {
        device, 0x2000u, 0x208u, D3DFMT_INDEX16, 0u, index_buffer_output, 0u
    };
    if (ac1_d3d9_dispatch(object_slot(device, 26), create_vertex_buffer_args, 7,
                          &getter_result) != 1 ||
        FAILED(static_cast<HRESULT>(getter_result)) || !guest_word(vertex_buffer_output) ||
        ac1_d3d9_dispatch(object_slot(device, 27), create_index_buffer_args, 7,
                          &getter_result) != 1 ||
        FAILED(static_cast<HRESULT>(getter_result)) || !guest_word(index_buffer_output)) {
        std::fprintf(stderr, "native vertex/index buffer creation failed: %s (HRESULT=0x%08x)\n",
                     ac1_d3d9_last_error(), getter_result);
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 20;
    }
    const uint32_t vertex_buffer = guest_word(vertex_buffer_output);
    const uint32_t index_buffer = guest_word(index_buffer_output);
    if (ac1_d3d9_argument_count(object_slot(vertex_buffer, 2)) != 1 ||
        ac1_d3d9_argument_count(object_slot(index_buffer, 2)) != 1) {
        std::fprintf(stderr, "created D3D9 buffers did not receive the right proxy kinds\n");
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 21;
    }
    const uint32_t unsupported_shared_args[] = {
        device, 0x10000u, 0x208u, 0u, 0u, vertex_buffer_output, 1u
    };
    if (ac1_d3d9_dispatch(object_slot(device, 26), unsupported_shared_args, 7,
                          &getter_result) != -2) {
        std::fprintf(stderr, "non-null shared buffer handle was not trapped explicitly\n");
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 22;
    }

    /* Exercise real guest32 staging through native dynamic vertex/index
     * buffers. Read back with a second game-style NOOVERWRITE lock. */
    const uint32_t vertex_lock_output = allocate_guest(nullptr, 4, 4);
    uint8_t vertex_pattern[32];
    for (uint32_t i = 0; i < sizeof(vertex_pattern); ++i)
        vertex_pattern[i] = static_cast<uint8_t>(0x31u + i * 7u);
    const uint32_t vertex_discard_lock[] = {
        vertex_buffer, 0u, sizeof(vertex_pattern), vertex_lock_output, D3DLOCK_DISCARD
    };
    if (ac1_d3d9_argument_count(object_slot(vertex_buffer, 11)) != 5 ||
        ac1_d3d9_argument_count(object_slot(vertex_buffer, 12)) != 1 ||
        ac1_d3d9_dispatch(object_slot(vertex_buffer, 11), vertex_discard_lock, 5,
                          &getter_result) != 1 || FAILED(static_cast<HRESULT>(getter_result))) {
        std::fprintf(stderr, "native vertex buffer DISCARD Lock failed: %s (HRESULT=0x%08x)\n",
                     ac1_d3d9_last_error(), getter_result);
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 28;
    }
    const uint32_t vertex_lock_guest_address = guest_word(vertex_lock_output);
    if (!vertex_lock_guest_address || vertex_lock_guest_address == 0xFFFFFFFFu ||
        !resolve_guest(nullptr, vertex_lock_guest_address, sizeof(vertex_pattern),
                       AC1_D3D9_GUEST_WRITE)) {
        std::fprintf(stderr, "vertex Lock did not return a checked guest staging span\n");
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 29;
    }
    const uint32_t rejected_lock_output = allocate_guest(nullptr, 4, 4);
    const uint32_t rejected_lock_sentinel = 0x6a6a6a6au;
    std::memcpy(resolve_guest(nullptr, rejected_lock_output, 4, AC1_D3D9_GUEST_WRITE),
                &rejected_lock_sentinel, sizeof(rejected_lock_sentinel));
    const size_t allocations_with_first_lock = live_guest_allocations.size();
    const uint32_t second_vertex_lock[] = {
        vertex_buffer, 0u, sizeof(vertex_pattern), rejected_lock_output, 0u
    };
    if (ac1_d3d9_dispatch(object_slot(vertex_buffer, 11), second_vertex_lock, 5,
                          &getter_result) != -2 ||
        guest_word(rejected_lock_output) != rejected_lock_sentinel ||
        live_guest_allocations.size() != allocations_with_first_lock) {
        std::fprintf(stderr, "second outstanding Lock was not rejected as unsupported\n");
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 57;
    }
    std::memcpy(resolve_guest(nullptr, vertex_lock_guest_address, sizeof(vertex_pattern),
                              AC1_D3D9_GUEST_WRITE), vertex_pattern,
                sizeof(vertex_pattern));
    const uint32_t vertex_this[] = {vertex_buffer};
    if (ac1_d3d9_dispatch(object_slot(vertex_buffer, 12), vertex_this, 1,
                          &getter_result) != 1 || FAILED(static_cast<HRESULT>(getter_result))) {
        std::fprintf(stderr, "native vertex buffer Unlock/writeback failed: %s (HRESULT=0x%08x)\n",
                     ac1_d3d9_last_error(), getter_result);
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 30;
    }
    const size_t allocations_after_unlock = live_guest_allocations.size();
    const uint32_t unsupported_lock_flags[] = {
        vertex_buffer, 0u, sizeof(vertex_pattern), rejected_lock_output, D3DLOCK_NOSYSLOCK
    };
    if (ac1_d3d9_dispatch(object_slot(vertex_buffer, 11), unsupported_lock_flags, 5,
                          &getter_result) != -2 ||
        guest_word(rejected_lock_output) != rejected_lock_sentinel ||
        live_guest_allocations.size() != allocations_after_unlock) {
        std::fprintf(stderr, "unsupported Lock flags were not reported as unsupported\n");
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 58;
    }
    const uint32_t vertex_readback_lock[] = {
        vertex_buffer, 0u, sizeof(vertex_pattern), vertex_lock_output, D3DLOCK_NOOVERWRITE
    };
    if (ac1_d3d9_dispatch(object_slot(vertex_buffer, 11), vertex_readback_lock, 5,
                          &getter_result) != 1 || FAILED(static_cast<HRESULT>(getter_result))) {
        std::fprintf(stderr, "native vertex buffer NOOVERWRITE readback Lock failed: %s (HRESULT=0x%08x)\n",
                     ac1_d3d9_last_error(), getter_result);
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 31;
    }
    const uint32_t vertex_readback_address = guest_word(vertex_lock_output);
    void *vertex_readback = resolve_guest(nullptr, vertex_readback_address,
        sizeof(vertex_pattern), AC1_D3D9_GUEST_READ);
    if (!vertex_readback || std::memcmp(vertex_readback, vertex_pattern,
                                        sizeof(vertex_pattern)) != 0 ||
        ac1_d3d9_dispatch(object_slot(vertex_buffer, 12), vertex_this, 1,
                          &getter_result) != 1 || FAILED(static_cast<HRESULT>(getter_result))) {
        std::fprintf(stderr, "native vertex buffer write/readback mismatch: %s\n",
                     ac1_d3d9_last_error());
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 32;
    }

    /* Invalid ranges must fail before allocating staging or leaving a lock
     * active; a following valid zero-size lock must still work. */
    const uint32_t invalid_range_output = allocate_guest(nullptr, 4, 4);
    const size_t live_before_bad_ranges = live_guest_allocations.size();
    const uint32_t sentinel = 0xa5a5a5a5u;
    std::memcpy(resolve_guest(nullptr, invalid_range_output, 4, AC1_D3D9_GUEST_WRITE),
                &sentinel, sizeof(sentinel));
    const uint32_t past_end_lock[] = {
        vertex_buffer, 0xfff8u, 16u, invalid_range_output, 0u
    };
    const uint32_t wrapping_range_lock[] = {
        vertex_buffer, 0xfffffff0u, 0x40u, invalid_range_output, 0u
    };
    if (ac1_d3d9_dispatch(object_slot(vertex_buffer, 11), past_end_lock, 5,
                          &getter_result) != 1 ||
        getter_result != static_cast<uint32_t>(D3DERR_INVALIDCALL) ||
        guest_word(invalid_range_output) != sentinel ||
        live_guest_allocations.size() != live_before_bad_ranges ||
        ac1_d3d9_dispatch(object_slot(vertex_buffer, 11), wrapping_range_lock, 5,
                          &getter_result) != 1 ||
        getter_result != static_cast<uint32_t>(D3DERR_INVALIDCALL) ||
        guest_word(invalid_range_output) != sentinel ||
        live_guest_allocations.size() != live_before_bad_ranges) {
        std::fprintf(stderr, "out-of-bounds/wrapping VB Lock was not rejected atomically: %s\n",
                     ac1_d3d9_last_error());
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 38;
    }
    const uint32_t vertex_zero_size_lock[] = {
        vertex_buffer, 0xfff0u, 0u, invalid_range_output, 0u
    };
    if (ac1_d3d9_dispatch(object_slot(vertex_buffer, 11), vertex_zero_size_lock, 5,
                          &getter_result) != 1 || FAILED(static_cast<HRESULT>(getter_result))) {
        std::fprintf(stderr, "zero-size VB Lock at tail did not lock the remaining range: %s\n",
                     ac1_d3d9_last_error());
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 39;
    }
    const uint32_t zero_size_guest = guest_word(invalid_range_output);
    void *zero_size_tail = resolve_guest(nullptr, zero_size_guest, 16, AC1_D3D9_GUEST_WRITE);
    if (!zero_size_tail) {
        std::fprintf(stderr, "zero-size VB Lock did not stage the full remaining tail\n");
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 40;
    }
    static_cast<uint8_t *>(zero_size_tail)[15] = 0x6du;
    if (ac1_d3d9_dispatch(object_slot(vertex_buffer, 12), vertex_this, 1,
                          &getter_result) != 1 || FAILED(static_cast<HRESULT>(getter_result))) {
        std::fprintf(stderr, "zero-size VB Lock tail Unlock failed: %s\n",
                     ac1_d3d9_last_error());
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 41;
    }

    /* READONLY must expose a staging copy without writing guest modifications
     * back to a real managed buffer. */
    const uint32_t readonly_buffer_output = allocate_guest(nullptr, 4, 4);
    const uint32_t create_readonly_buffer_args[] = {
        device, 64u, 0u, 0u, D3DPOOL_MANAGED, readonly_buffer_output, 0u
    };
    if (ac1_d3d9_dispatch(object_slot(device, 26), create_readonly_buffer_args, 7,
                          &getter_result) != 1 || FAILED(static_cast<HRESULT>(getter_result))) {
        std::fprintf(stderr, "managed test VB creation failed: %s (HRESULT=0x%08x)\n",
                     ac1_d3d9_last_error(), getter_result);
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 42;
    }
    const uint32_t readonly_buffer = guest_word(readonly_buffer_output);
    const uint32_t readonly_lock_output = allocate_guest(nullptr, 4, 4);
    uint8_t readonly_original[64];
    uint8_t readonly_modified[64];
    for (uint32_t i = 0; i < sizeof(readonly_original); ++i) {
        readonly_original[i] = static_cast<uint8_t>(0x5bu + i * 3u);
        readonly_modified[i] = static_cast<uint8_t>(0xc3u - i * 5u);
    }
    const uint32_t readonly_init_lock[] = {
        readonly_buffer, 0u, sizeof(readonly_original), readonly_lock_output, 0u
    };
    if (ac1_d3d9_dispatch(object_slot(readonly_buffer, 11), readonly_init_lock, 5,
                          &getter_result) != 1 || FAILED(static_cast<HRESULT>(getter_result))) {
        std::fprintf(stderr, "managed test VB initialization Lock failed: %s\n",
                     ac1_d3d9_last_error());
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 43;
    }
    std::memcpy(resolve_guest(nullptr, guest_word(readonly_lock_output), sizeof(readonly_original),
                              AC1_D3D9_GUEST_WRITE), readonly_original,
                sizeof(readonly_original));
    const uint32_t readonly_buffer_this[] = {readonly_buffer};
    if (ac1_d3d9_dispatch(object_slot(readonly_buffer, 12), readonly_buffer_this, 1,
                          &getter_result) != 1 || FAILED(static_cast<HRESULT>(getter_result))) {
        std::fprintf(stderr, "managed test VB initialization Unlock failed: %s\n",
                     ac1_d3d9_last_error());
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 44;
    }
    const uint32_t readonly_lock[] = {
        readonly_buffer, 0u, sizeof(readonly_modified), readonly_lock_output, D3DLOCK_READONLY
    };
    if (ac1_d3d9_dispatch(object_slot(readonly_buffer, 11), readonly_lock, 5,
                          &getter_result) != 1 || FAILED(static_cast<HRESULT>(getter_result))) {
        std::fprintf(stderr, "managed test VB READONLY Lock failed: %s\n",
                     ac1_d3d9_last_error());
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 45;
    }
    std::memcpy(resolve_guest(nullptr, guest_word(readonly_lock_output), sizeof(readonly_modified),
                              AC1_D3D9_GUEST_WRITE), readonly_modified,
                sizeof(readonly_modified));
    if (ac1_d3d9_dispatch(object_slot(readonly_buffer, 12), readonly_buffer_this, 1,
                          &getter_result) != 1 || FAILED(static_cast<HRESULT>(getter_result))) {
        std::fprintf(stderr, "managed test VB READONLY Unlock failed: %s\n",
                     ac1_d3d9_last_error());
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 46;
    }
    if (ac1_d3d9_dispatch(object_slot(readonly_buffer, 11), readonly_init_lock, 5,
                          &getter_result) != 1 || FAILED(static_cast<HRESULT>(getter_result)) ||
        std::memcmp(resolve_guest(nullptr, guest_word(readonly_lock_output), sizeof(readonly_original),
                                 AC1_D3D9_GUEST_READ), readonly_original,
                    sizeof(readonly_original)) != 0 ||
        ac1_d3d9_dispatch(object_slot(readonly_buffer, 12), readonly_buffer_this, 1,
                          &getter_result) != 1 || FAILED(static_cast<HRESULT>(getter_result))) {
        std::fprintf(stderr, "READONLY staging modification changed native VB contents: %s\n",
                     ac1_d3d9_last_error());
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 47;
    }
    uint32_t readonly_release_count = 0;
    if (ac1_d3d9_dispatch(object_slot(readonly_buffer, 2), readonly_buffer_this, 1,
                          &readonly_release_count) != 1 || readonly_release_count != 0) {
        std::fprintf(stderr, "managed READONLY test VB release failed\n");
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 48;
    }

    /* Last proxy Release must Unlock the native buffer and free its staging. */
    const uint32_t abandoned_release_output = allocate_guest(nullptr, 4, 4);
    const uint32_t create_abandoned_release_args[] = {
        device, 256u, 0x208u, 0u, D3DPOOL_DEFAULT, abandoned_release_output, 0u
    };
    if (ac1_d3d9_dispatch(object_slot(device, 26), create_abandoned_release_args, 7,
                          &getter_result) != 1 || FAILED(static_cast<HRESULT>(getter_result))) {
        std::fprintf(stderr, "abandoned Release test VB creation failed: %s\n",
                     ac1_d3d9_last_error());
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 49;
    }
    const uint32_t abandoned_release_buffer = guest_word(abandoned_release_output);
    const uint32_t abandoned_release_lock_output = allocate_guest(nullptr, 4, 4);
    const uint32_t abandoned_release_lock[] = {
        abandoned_release_buffer, 0u, 32u, abandoned_release_lock_output, D3DLOCK_DISCARD
    };
    if (ac1_d3d9_dispatch(object_slot(abandoned_release_buffer, 11),
                          abandoned_release_lock, 5, &getter_result) != 1 ||
        FAILED(static_cast<HRESULT>(getter_result))) {
        std::fprintf(stderr, "abandoned Release test Lock failed: %s\n",
                     ac1_d3d9_last_error());
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 50;
    }
    const uint32_t abandoned_release_staging = guest_word(abandoned_release_lock_output);
    if (!guest_allocation_live(abandoned_release_staging)) {
        std::fprintf(stderr, "abandoned Release test staging was not tracked\n");
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 51;
    }
    const uint32_t abandoned_release_this[] = {abandoned_release_buffer};
    uint32_t abandoned_release_count = UINT32_MAX;
    if (ac1_d3d9_dispatch(object_slot(abandoned_release_buffer, 2),
                          abandoned_release_this, 1, &abandoned_release_count) != 1 ||
        abandoned_release_count != 0 || guest_allocation_live(abandoned_release_staging)) {
        std::fprintf(stderr, "final Release did not clean the locked buffer/staging: %s\n",
                     ac1_d3d9_last_error());
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 52;
    }

    const uint32_t index_lock_output = allocate_guest(nullptr, 4, 4);
    uint16_t index_pattern[8] = {0, 1, 3, 7, 15, 31, 63, 127};
    const uint32_t index_discard_lock[] = {
        index_buffer, 0u, sizeof(index_pattern), index_lock_output, D3DLOCK_DISCARD
    };
    if (ac1_d3d9_argument_count(object_slot(index_buffer, 11)) != 5 ||
        ac1_d3d9_argument_count(object_slot(index_buffer, 12)) != 1 ||
        ac1_d3d9_dispatch(object_slot(index_buffer, 11), index_discard_lock, 5,
                          &getter_result) != 1 || FAILED(static_cast<HRESULT>(getter_result))) {
        std::fprintf(stderr, "native index buffer DISCARD Lock failed: %s (HRESULT=0x%08x)\n",
                     ac1_d3d9_last_error(), getter_result);
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 33;
    }
    const uint32_t index_lock_guest_address = guest_word(index_lock_output);
    if (!index_lock_guest_address ||
        !resolve_guest(nullptr, index_lock_guest_address, sizeof(index_pattern),
                       AC1_D3D9_GUEST_WRITE)) {
        std::fprintf(stderr, "index Lock did not return a checked guest staging span\n");
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 34;
    }
    std::memcpy(resolve_guest(nullptr, index_lock_guest_address, sizeof(index_pattern),
                              AC1_D3D9_GUEST_WRITE), index_pattern,
                sizeof(index_pattern));
    const uint32_t index_this[] = {index_buffer};
    if (ac1_d3d9_dispatch(object_slot(index_buffer, 12), index_this, 1,
                          &getter_result) != 1 || FAILED(static_cast<HRESULT>(getter_result))) {
        std::fprintf(stderr, "native index buffer Unlock/writeback failed: %s (HRESULT=0x%08x)\n",
                     ac1_d3d9_last_error(), getter_result);
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 35;
    }
    const uint32_t index_readback_lock[] = {
        index_buffer, 0u, sizeof(index_pattern), index_lock_output, D3DLOCK_NOOVERWRITE
    };
    if (ac1_d3d9_dispatch(object_slot(index_buffer, 11), index_readback_lock, 5,
                          &getter_result) != 1 || FAILED(static_cast<HRESULT>(getter_result))) {
        std::fprintf(stderr, "native index buffer NOOVERWRITE readback Lock failed: %s (HRESULT=0x%08x)\n",
                     ac1_d3d9_last_error(), getter_result);
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 36;
    }
    const uint32_t index_readback_address = guest_word(index_lock_output);
    void *index_readback = resolve_guest(nullptr, index_readback_address,
        sizeof(index_pattern), AC1_D3D9_GUEST_READ);
    if (!index_readback || std::memcmp(index_readback, index_pattern,
                                       sizeof(index_pattern)) != 0 ||
        ac1_d3d9_dispatch(object_slot(index_buffer, 12), index_this, 1,
                          &getter_result) != 1 || FAILED(static_cast<HRESULT>(getter_result))) {
        std::fprintf(stderr, "native index buffer write/readback mismatch: %s\n",
                     ac1_d3d9_last_error());
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 37;
    }
    std::printf("native DXVK dynamic VB/IB staging writeback/readback passed\n");
    const uint32_t declaration_output = allocate_guest(nullptr, 4, 4);
    const D3DVERTEXELEMENT9 declaration_elements[] = {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT,
         D3DDECLUSAGE_POSITION, 0},
        D3DDECL_END()
    };
    const uint32_t declaration_elements_address = allocate_guest(
        nullptr, sizeof(declaration_elements), 4);
    std::memcpy(resolve_guest(nullptr, declaration_elements_address,
                              sizeof(declaration_elements), AC1_D3D9_GUEST_WRITE),
                declaration_elements, sizeof(declaration_elements));
    const uint32_t declaration_args[] = {
        device, declaration_elements_address, declaration_output
    };
    const uint32_t unterminated_output = allocate_guest(nullptr, 4, 4);
    const uint32_t unterminated_address = allocate_guest(
        nullptr, (MAXD3DDECLLENGTH + 1u) * sizeof(D3DVERTEXELEMENT9), 4);
    std::array<D3DVERTEXELEMENT9, MAXD3DDECLLENGTH + 1u> unterminated_elements{};
    for (auto &element : unterminated_elements) {
        element = {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT,
                   D3DDECLUSAGE_POSITION, 0};
    }
    std::memcpy(resolve_guest(nullptr, unterminated_address,
                              static_cast<uint32_t>(sizeof(unterminated_elements)),
                              AC1_D3D9_GUEST_WRITE), unterminated_elements.data(),
                sizeof(unterminated_elements));
    const uint32_t unterminated_args[] = {
        device, unterminated_address, unterminated_output
    };
    const uint32_t unmapped_declaration_args[] = {
        device, 0xfffffff8u, unterminated_output
    };
    if (ac1_d3d9_dispatch(object_slot(device, 86), unterminated_args, 3,
                          &getter_result) != 1 ||
        getter_result != static_cast<uint32_t>(D3DERR_INVALIDCALL) ||
        guest_word(unterminated_output) != 0 ||
        ac1_d3d9_dispatch(object_slot(device, 86), unmapped_declaration_args, 3,
                          &getter_result) != 1 ||
        getter_result != static_cast<uint32_t>(D3DERR_INVALIDCALL) ||
        ac1_d3d9_dispatch(object_slot(device, 86), declaration_args, 3,
                          &getter_result) != 1 ||
        FAILED(static_cast<HRESULT>(getter_result)) || !guest_word(declaration_output)) {
        std::fprintf(stderr, "CreateVertexDeclaration smoke failed: %s (HRESULT=0x%08x)\n",
                     ac1_d3d9_last_error(), getter_result);
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 30;
    }
    const uint32_t declaration = guest_word(declaration_output);
    if (ac1_d3d9_argument_count(object_slot(declaration, 2)) != 1 ||
        ac1_d3d9_argument_count(object_slot(declaration, 4)) != 3) {
        std::fprintf(stderr, "vertex declaration proxy kind was not registered\n");
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 31;
    }
    const uint8_t iid_vertex_declaration[16] = {
        0x9c, 0xc5, 0x13, 0xdd, 0xfa, 0x36, 0x98, 0x40,
        0xa8, 0xfb, 0xc7, 0xed, 0x39, 0xdc, 0x85, 0x46
    };
    const uint32_t declaration_iid_address = allocate_guest(
        nullptr, sizeof(iid_vertex_declaration), 4);
    const uint32_t declaration_query_output = allocate_guest(nullptr, 4, 4);
    std::memcpy(resolve_guest(nullptr, declaration_iid_address,
                              sizeof(iid_vertex_declaration), AC1_D3D9_GUEST_WRITE),
                iid_vertex_declaration, sizeof(iid_vertex_declaration));
    const uint32_t declaration_query_args[] = {
        declaration, declaration_iid_address, declaration_query_output
    };
    if (ac1_d3d9_dispatch(object_slot(declaration, 0), declaration_query_args, 3,
                          &getter_result) != 1 ||
        FAILED(static_cast<HRESULT>(getter_result)) ||
        guest_word(declaration_query_output) != declaration) {
        std::fprintf(stderr, "vertex declaration QueryInterface identity failed: %s\n",
                     ac1_d3d9_last_error());
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 32;
    }
    const uint32_t declaration_count_output = allocate_guest(nullptr, 4, 4);
    const uint32_t declaration_elements_output = allocate_guest(
        nullptr, sizeof(declaration_elements), 4);
    const uint32_t zero_count = 0;
    std::memcpy(resolve_guest(nullptr, declaration_count_output, sizeof(zero_count),
                              AC1_D3D9_GUEST_WRITE), &zero_count, sizeof(zero_count));
    const uint32_t get_declaration_args[] = {
        declaration, declaration_elements_output, declaration_count_output
    };
    if (ac1_d3d9_dispatch(object_slot(declaration, 4), get_declaration_args, 3,
                          &getter_result) != 1 ||
        FAILED(static_cast<HRESULT>(getter_result)) ||
        guest_word(declaration_count_output) !=
            sizeof(declaration_elements) / sizeof(declaration_elements[0]) ||
        std::memcmp(resolve_guest(nullptr, declaration_elements_output,
                                  sizeof(declaration_elements), AC1_D3D9_GUEST_READ),
                    declaration_elements, sizeof(declaration_elements)) != 0) {
        std::fprintf(stderr, "GetDeclaration did not round-trip the declaration: %s\n",
                     ac1_d3d9_last_error());
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 35;
    }
    const uint32_t count_query_args[] = {declaration, 0u, declaration_count_output};
    if (ac1_d3d9_dispatch(object_slot(declaration, 4), count_query_args, 3,
                          &getter_result) != 1 ||
        FAILED(static_cast<HRESULT>(getter_result)) ||
        guest_word(declaration_count_output) !=
            sizeof(declaration_elements) / sizeof(declaration_elements[0])) {
        std::fprintf(stderr, "GetDeclaration count-only query failed: %s\n",
                     ac1_d3d9_last_error());
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 36;
    }
    const uint32_t unmapped_declaration_output[] = {
        declaration, 0xfffffff8u, declaration_count_output
    };
    const uint32_t unmapped_declaration_count[] = {
        declaration, declaration_elements_output, 0xfffffff0u
    };
    if (ac1_d3d9_dispatch(object_slot(declaration, 4),
                          unmapped_declaration_output, 3, &getter_result) != 1 ||
        getter_result != static_cast<uint32_t>(D3DERR_INVALIDCALL) ||
        ac1_d3d9_dispatch(object_slot(declaration, 4),
                          unmapped_declaration_count, 3, &getter_result) != 1 ||
        getter_result != static_cast<uint32_t>(D3DERR_INVALIDCALL)) {
        std::fprintf(stderr, "GetDeclaration accepted unmapped output memory\n");
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 37;
    }
    const uint32_t declaration_this[] = {declaration};
    if (ac1_d3d9_dispatch(object_slot(declaration, 2), declaration_this, 1,
                          &references) != 1 || references != 1) {
        std::fprintf(stderr, "vertex declaration reference balance failed\n");
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 33;
    }
    if (!run_native_indexed_triangle_smoke(device, vertex_buffer, index_buffer, texture)) {
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 74;
    }

    const uint32_t get_render_target = object_slot(device, 38);
    const uint32_t get_depth_stencil = object_slot(device, 40);
    const uint32_t render_output = allocate_guest(nullptr, 4, 4);
    const uint32_t depth_output = allocate_guest(nullptr, 4, 4);
    const uint32_t render_args[] = {device, 0, render_output};
    const uint32_t depth_args[] = {device, depth_output};
    if (ac1_d3d9_argument_count(get_render_target) != 3 ||
        ac1_d3d9_argument_count(get_depth_stencil) != 2 ||
        ac1_d3d9_argument_count(object_slot(device, 37)) != 3 ||
        ac1_d3d9_argument_count(object_slot(device, 39)) != 2 ||
        ac1_d3d9_argument_count(object_slot(device, 3)) != 1 ||
        ac1_d3d9_dispatch(get_render_target, render_args, 3, &getter_result) != 1 ||
        FAILED(static_cast<HRESULT>(getter_result)) || !guest_word(render_output) ||
        ac1_d3d9_dispatch(get_depth_stencil, depth_args, 2, &getter_result) != 1 ||
        FAILED(static_cast<HRESULT>(getter_result)) || !guest_word(depth_output)) {
        std::fprintf(stderr, "device surface getter smoke failed: %s (HRESULT=0x%08x)\n",
                     ac1_d3d9_last_error(), getter_result);
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 11;
    }

    const uint32_t bind_render_target_args[] = {device, 0u, guest_word(render_output)};
    const uint32_t clear_additional_target_args[] = {device, 1u, 0u};
    if (ac1_d3d9_dispatch(object_slot(device, 37), bind_render_target_args, 3,
                          &getter_result) != 1 ||
        FAILED(static_cast<HRESULT>(getter_result)) ||
        ac1_d3d9_dispatch(object_slot(device, 37), clear_additional_target_args, 3,
                          &getter_result) != 1 ||
        FAILED(static_cast<HRESULT>(getter_result))) {
        std::fprintf(stderr, "native SetRenderTarget proxy/null forwarding failed: %s (HRESULT=0x%08x)\n",
                     ac1_d3d9_last_error(), getter_result);
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 76;
    }

    const uint32_t bind_depth_args[] = {device, guest_word(depth_output)};
    const uint32_t unbind_depth_args[] = {device, 0u};
    if (ac1_d3d9_dispatch(object_slot(device, 39), bind_depth_args, 2,
                          &getter_result) != 1 ||
        FAILED(static_cast<HRESULT>(getter_result)) ||
        ac1_d3d9_dispatch(object_slot(device, 39), unbind_depth_args, 2,
                          &getter_result) != 1 ||
        FAILED(static_cast<HRESULT>(getter_result))) {
        std::fprintf(stderr, "native SetDepthStencilSurface proxy/null forwarding failed: %s (HRESULT=0x%08x)\n",
                     ac1_d3d9_last_error(), getter_result);
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 75;
    }

    const uint32_t surface = guest_word(render_output);
    const uint32_t color_fill_args[] = {device, surface, 0u, 0xff204060u};
    const uint32_t fill_rect_address = allocate_guest(nullptr, sizeof(RECT), 4);
    const RECT fill_rect{0, 0, 16, 16};
    std::memcpy(resolve_guest(nullptr, fill_rect_address, sizeof(fill_rect),
                              AC1_D3D9_GUEST_WRITE), &fill_rect, sizeof(fill_rect));
    const uint32_t color_fill_rect_args[] = {
        device, surface, fill_rect_address, 0xff102030u
    };
    const uint32_t bad_color_fill_args[] = {device, device, 0u, 0u};
    if (ac1_d3d9_argument_count(object_slot(device, 35)) != 4 ||
        ac1_d3d9_dispatch(object_slot(device, 35), color_fill_args, 4,
                          &getter_result) != 1 ||
        FAILED(static_cast<HRESULT>(getter_result)) ||
        ac1_d3d9_dispatch(object_slot(device, 35), color_fill_rect_args, 4,
                          &getter_result) != 1 ||
        FAILED(static_cast<HRESULT>(getter_result))) {
        std::fprintf(stderr, "native ColorFill smoke failed: %s (HRESULT=0x%08x)\n",
                     ac1_d3d9_last_error(), getter_result);
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 26;
    }
    if (ac1_d3d9_dispatch(object_slot(device, 35), bad_color_fill_args, 4,
                          &getter_result) != 1 ||
        getter_result != static_cast<uint32_t>(D3DERR_INVALIDCALL)) {
        std::fprintf(stderr, "ColorFill did not reject a non-surface guest proxy\n");
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 27;
    }
    const D3DVIEWPORT9 viewport{0, 0, 640u, 480u, 0.0f, 1.0f};
    const uint32_t viewport_address = allocate_guest(nullptr, sizeof(viewport), 4);
    std::memcpy(resolve_guest(nullptr, viewport_address, sizeof(viewport),
                              AC1_D3D9_GUEST_WRITE), &viewport, sizeof(viewport));
    const uint32_t set_viewport_args[] = {device, viewport_address};
    const uint32_t unmapped_viewport_args[] = {device, 0xfffffff0u};
    if (ac1_d3d9_argument_count(object_slot(device, 47)) != 2 ||
        ac1_d3d9_dispatch(object_slot(device, 47), set_viewport_args, 2,
                          &getter_result) != 1 ||
        FAILED(static_cast<HRESULT>(getter_result))) {
        std::fprintf(stderr, "native SetViewport smoke failed: %s (HRESULT=0x%08x)\n",
                     ac1_d3d9_last_error(), getter_result);
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 28;
    }
    if (ac1_d3d9_dispatch(object_slot(device, 47), unmapped_viewport_args, 2,
                          &getter_result) != 1 ||
        getter_result != static_cast<uint32_t>(D3DERR_INVALIDCALL)) {
        std::fprintf(stderr, "SetViewport did not reject an unmapped guest structure\n");
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 29;
    }
    const uint32_t surface_desc_token = object_slot(surface, 12);
    const uint32_t desc_address = allocate_guest(nullptr, 32, 4);
    const uint32_t desc_args[] = {surface, desc_address};
    if (ac1_d3d9_argument_count(surface_desc_token) != 2 ||
        ac1_d3d9_argument_count(object_slot(surface, 3)) != 2 ||
        ac1_d3d9_dispatch(surface_desc_token, desc_args, 2, &getter_result) != 1 ||
        FAILED(static_cast<HRESULT>(getter_result))) {
        std::fprintf(stderr, "surface GetDesc smoke failed: %s (HRESULT=0x%08x)\n",
                     ac1_d3d9_last_error(), getter_result);
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 12;
    }
    uint32_t surface_type = 0, surface_width = 0, surface_height = 0;
    std::memcpy(&surface_type,
                resolve_guest(nullptr, desc_address + 4, 4, AC1_D3D9_GUEST_READ), 4);
    std::memcpy(&surface_width,
                resolve_guest(nullptr, desc_address + 24, 4, AC1_D3D9_GUEST_READ), 4);
    std::memcpy(&surface_height,
                resolve_guest(nullptr, desc_address + 28, 4, AC1_D3D9_GUEST_READ), 4);
    if (surface_type != D3DRTYPE_SURFACE || !surface_width || !surface_height) {
        std::fprintf(stderr, "surface GetDesc returned unexpected type/extent (%u, %u x %u)\n",
                     surface_type, surface_width, surface_height);
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 13;
    }

    const uint32_t surface_device_output = allocate_guest(nullptr, 4, 4);
    const uint32_t get_surface_device_args[] = {surface, surface_device_output};
    if (ac1_d3d9_dispatch(object_slot(surface, 3), get_surface_device_args, 2,
                          &getter_result) != 1 ||
        FAILED(static_cast<HRESULT>(getter_result)) ||
        guest_word(surface_device_output) != device) {
        std::fprintf(stderr, "surface GetDevice did not return the existing device proxy: %s "
                             "(HRESULT=0x%08x, guest=0x%08x)\n",
                     ac1_d3d9_last_error(), getter_result,
                     guest_word(surface_device_output));
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 24;
    }
    const uint32_t device_this_for_getdevice[] = {device};
    if (ac1_d3d9_dispatch(object_slot(device, 2), device_this_for_getdevice, 1,
                          &references) != 1 || references != 1) {
        std::fprintf(stderr, "surface GetDevice reference balance failed\n");
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 25;
    }

    const uint8_t iid_unknown[16] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46
    };
    const uint32_t iid_address = allocate_guest(nullptr, sizeof(iid_unknown), 4);
    const uint32_t queried_surface_output = allocate_guest(nullptr, 4, 4);
    std::memcpy(resolve_guest(nullptr, iid_address, sizeof(iid_unknown), AC1_D3D9_GUEST_WRITE),
                iid_unknown, sizeof(iid_unknown));
    const uint32_t query_surface_args[] = {surface, iid_address, queried_surface_output};
    if (ac1_d3d9_argument_count(object_slot(surface, 0)) != 3 ||
        ac1_d3d9_dispatch(object_slot(surface, 0), query_surface_args, 3,
                          &getter_result) != 1 ||
        FAILED(static_cast<HRESULT>(getter_result)) ||
        guest_word(queried_surface_output) != surface) {
        std::fprintf(stderr, "surface IUnknown QueryInterface identity failed: %s\n",
                     ac1_d3d9_last_error());
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 14;
    }

    const uint32_t surface_addref = object_slot(surface, 1);
    const uint32_t surface_release = object_slot(surface, 2);
    const uint32_t surface_this[] = {surface};
    if (ac1_d3d9_dispatch(surface_addref, surface_this, 1, &references) != 1 ||
        references != 3 ||
        ac1_d3d9_dispatch(surface_release, surface_this, 1, &references) != 1 ||
        references != 2 ||
        ac1_d3d9_dispatch(surface_release, surface_this, 1, &references) != 1 ||
        references != 1 ||
        ac1_d3d9_dispatch(surface_release, surface_this, 1, &references) != 1 ||
        references != 0) {
        std::fprintf(stderr, "surface COM reference smoke failed: %s\n",
                     ac1_d3d9_last_error());
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 15;
    }

    const uint32_t depth_surface = guest_word(depth_output);
    const uint32_t depth_release = object_slot(depth_surface, 2);
    const uint32_t depth_this[] = {depth_surface};
    if (ac1_d3d9_dispatch(depth_release, depth_this, 1, &references) != 1 || references != 0) {
        std::fprintf(stderr, "depth surface release smoke failed: %s\n",
                     ac1_d3d9_last_error());
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 16;
    }
    const uint32_t shutdown_buffer_output = allocate_guest(nullptr, 4, 4);
    const uint32_t create_shutdown_buffer_args[] = {
        device, 256u, 0x208u, 0u, D3DPOOL_DEFAULT, shutdown_buffer_output, 0u
    };
    if (ac1_d3d9_dispatch(object_slot(device, 26), create_shutdown_buffer_args, 7,
                          &getter_result) != 1 || FAILED(static_cast<HRESULT>(getter_result))) {
        std::fprintf(stderr, "shutdown cleanup test VB creation failed: %s\n",
                     ac1_d3d9_last_error());
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 53;
    }
    const uint32_t shutdown_buffer = guest_word(shutdown_buffer_output);
    const uint32_t shutdown_lock_output = allocate_guest(nullptr, 4, 4);
    const uint32_t shutdown_lock[] = {
        shutdown_buffer, 0u, 32u, shutdown_lock_output, D3DLOCK_DISCARD
    };
    if (ac1_d3d9_dispatch(object_slot(shutdown_buffer, 11), shutdown_lock, 5,
                          &getter_result) != 1 || FAILED(static_cast<HRESULT>(getter_result))) {
        std::fprintf(stderr, "shutdown cleanup test Lock failed: %s\n",
                     ac1_d3d9_last_error());
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 54;
    }
    const uint32_t shutdown_staging = guest_word(shutdown_lock_output);
    if (!guest_allocation_live(shutdown_staging)) {
        std::fprintf(stderr, "shutdown cleanup test staging was not tracked\n");
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 55;
    }
    const uint32_t device_release = object_slot(device, 2);
    const uint32_t device_this[] = {device};
    const uint32_t vertex_release_this[] = {vertex_buffer};
    const uint32_t index_release_this[] = {index_buffer};
    if (ac1_d3d9_dispatch(object_slot(vertex_buffer, 2), vertex_release_this, 1,
                          &references) != 1 || references != 0 ||
        ac1_d3d9_dispatch(object_slot(index_buffer, 2), index_release_this, 1,
                          &references) != 1 || references != 0) {
        std::fprintf(stderr, "vertex/index buffer Release failed: %s\n", ac1_d3d9_last_error());
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 23;
    }
    if (ac1_d3d9_dispatch(object_slot(declaration, 2), declaration_this, 1,
                          &references) != 1 || references != 0) {
        std::fprintf(stderr, "vertex declaration Release failed: %s\n",
                     ac1_d3d9_last_error());
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 34;
    }
    if (ac1_d3d9_dispatch(object_slot(texture, 2), texture_this, 1,
                          &references) != 1 || references != 0) {
        std::fprintf(stderr, "texture Release failed: %s\n", ac1_d3d9_last_error());
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 45;
    }
    if (ac1_d3d9_dispatch(device_release, device_this, 1, &references) != 1) {
        std::fprintf(stderr, "device release smoke failed: %s\n", ac1_d3d9_last_error());
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 17;
    }
    if (ac1_d3d9_dispatch(release_token, this_args, 1, &remaining) != 1 || remaining != 0) {
        std::fprintf(stderr, "final D3D9 Release dispatch failed: %s\n",
                     ac1_d3d9_last_error());
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 18;
    }
    DestroyWindow(hidden_window);
    ac1_d3d9_shutdown();
    if (std::find(freed_guest_allocations.begin(), freed_guest_allocations.end(),
                  shutdown_cube_locked.bits) == freed_guest_allocations.end()) {
        std::fprintf(stderr, "bridge shutdown did not free locked cube staging\n");
        return 72;
    }
    if (guest_allocation_live(shutdown_staging)) {
        std::fprintf(stderr, "bridge shutdown did not free locked buffer staging\n");
        return 56;
    }
    std::printf("native D3D9 bridge smoke passed; adapters=%u, windowed device surface=%ux%u\n",
                adapter_count, surface_width, surface_height);
    return run_fake_surface_smoke(callbacks) ? 0 : 19;
}
