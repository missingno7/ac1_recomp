// Portable D3DX9 subset. Independent implementation of the public CTAB wire
// format and D3DX contracts; no Wine implementation code is included here.
// Header-only so Android and the standalone bridge use exactly the tested code.
#pragma once
#include <d3d9.h>
#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace ac1_d3dx {
inline thread_local std::string diagnostic;
inline HRESULT fail(const std::string &s, HRESULT hr = D3DERR_INVALIDCALL)
{ diagnostic = s; return hr; }
struct TableDesc { const char *Creator; DWORD Version; UINT Constants; };
struct ConstantDesc {
    const char *Name;
    UINT RegisterSet, RegisterIndex, RegisterCount, Class, Type;
    UINT Rows, Columns, Elements, StructMembers, Bytes;
    const void *DefaultValue;
};
struct Vector { float v[4]; };
struct Matrix { float m[4][4]; };

class Table final : public IUnknown {
    struct Node {
        ConstantDesc d{};
        std::vector<std::unique_ptr<Node>> children;
    };
    std::atomic<ULONG> refs{1};
    std::vector<uint8_t> data;
    TableDesc desc{};
    std::vector<std::unique_ptr<Node>> roots;
    std::vector<Node *> handles;
    template<class T> T read(size_t off) const {
        if (off > data.size() || sizeof(T) > data.size() - off)
            throw std::runtime_error("CTAB field outside comment at offset " + std::to_string(off));
        T t; std::memcpy(&t, data.data() + off, sizeof(t)); return t;
    }
    const char *string(size_t off) const {
        if (off >= data.size() || !std::memchr(data.data()+off, 0, data.size()-off))
            throw std::runtime_error("CTAB unterminated string at offset " + std::to_string(off));
        return reinterpret_cast<const char *>(data.data()+off);
    }
    std::unique_ptr<Node> parse(UINT type, UINT name, UINT set, UINT index,
                                UINT limit, UINT &def, bool element, unsigned depth) {
        if (depth > 32 || handles.size() >= 65536)
            throw std::runtime_error("CTAB recursive type or node limit exceeded");
        auto n = std::make_unique<Node>();
        handles.push_back(n.get());
        auto &d = n->d;
        d.Name = string(name); d.RegisterSet = set; d.RegisterIndex = index;
        d.Class = read<uint16_t>(type); d.Type = read<uint16_t>(type+2);
        d.Rows = read<uint16_t>(type+4); d.Columns = read<uint16_t>(type+6);
        d.Elements = element ? 1 : read<uint16_t>(type+8);
        d.StructMembers = read<uint16_t>(type+10);
        UINT members = read<uint32_t>(type+12);
        if (set > 3 || d.Class > 5 || !d.Elements || !d.Rows || !d.Columns ||
            (d.Class != 5 && (d.Rows > 4 || d.Columns > 4)) ||
            (d.Class == 5 && !d.StructMembers) ||
            (set == 3 ? (d.Class != 4 || d.Type<10 || d.Type>14) : (d.Class == 4 || (d.Class != 5 && (d.Type < 1 || d.Type > 3)))))
            throw std::runtime_error("CTAB unsupported register set="+std::to_string(set)+
                " class="+std::to_string(d.Class)+" type="+std::to_string(d.Type)+" for "+d.Name);
        UINT start_default = def, registers = 0;
        if (d.Elements > 1 || d.Class == 5) {
            UINT count = d.Elements > 1 ? d.Elements : d.StructMembers;
            for (UINT i = 0; i < count; ++i) {
                bool array = d.Elements > 1;
                UINT absent_array_default = 0;
                auto child = parse(array ? type : read<uint32_t>(members+8*i+4),
                    array ? name : read<uint32_t>(members+8*i), set,
                    index+registers, limit, array && !start_default ? absent_array_default : def, array, depth+1);
                registers += child->d.RegisterCount;
                n->children.push_back(std::move(child));
            }
        } else {
            registers = set == 0 ? d.Rows*d.Columns : set == 3 ? 1 :
                d.Class == 3 ? d.Columns : d.Rows;
            // D3DX36 advances the member default offset even when the parent's
            // initial offset is zero. Later member descriptors expose that
            // location; SetDefaults still checks the top-level pointer only.
            def += 4 * (set == 0 ? d.Rows*d.Columns : registers*4);
        }
        d.RegisterCount = std::min(registers, limit > index ? limit-index : 0);
        uint64_t byte_count=uint64_t(4)*d.Rows*d.Columns*d.Elements;
        if (byte_count>1024*1024) throw std::runtime_error("CTAB logical value exceeds 1 MiB for " + std::string(d.Name));
        d.Bytes = UINT(byte_count);
        if (start_default) {
            UINT needed = std::max(d.Bytes, def-start_default);
            if (start_default > data.size() || needed > data.size()-start_default)
                throw std::runtime_error("CTAB truncated defaults for " + std::string(d.Name));
            d.DefaultValue = data.data()+start_default;
        }
        return n;
    }
    static const char *handle(Node *n) { return reinterpret_cast<const char *>(n); }
    Node *node(const char *h) const {
        if (!h) return nullptr;
        for (auto *n : handles) if (handle(n) == h) return n;
        // Host API permits a name as a handle. The guest adapter validates its
        // own string before calling GetConstantByName instead of trusting this.
        return lookup(nullptr, h);
    }
    Node *lookup(Node *parent, const char *name) const {
        if (!name || !*name) return nullptr;
        const auto &scope = parent ? parent->children : roots;
        const char *end = name + std::strcspn(name, "[.");
        Node *found = nullptr;
        for (const auto &n : scope) {
            const char *candidate = n->d.Name;
            if (std::strlen(candidate) == size_t(end-name) &&
                !std::strncmp(candidate,name,end-name)) { found=n.get(); break; }
        }
        if (!found) return nullptr;
        while (*end) {
            if (*end == '.') return lookup(found, end+1);
            if (*end++ != '[' || *end < '0' || *end > '9') return nullptr;
            UINT index=0;
            while (*end >= '0' && *end <= '9') {
                if (index > 65535) return nullptr;
                index = index*10 + (*end++-'0');
            }
            if (*end++ != ']' || index >= found->d.Elements) return nullptr;
            if (found->d.Elements > 1) found=found->children[index].get();
        }
        return found;
    }
    HRESULT write(IDirect3DDevice9 *dev, const ConstantDesc &d, UINT reg,
                  const void *values, UINT count) const {
        bool vs = (desc.Version >> 16) == 0xfffe;
        switch (d.RegisterSet) {
        case 0: return vs ? dev->SetVertexShaderConstantB(reg,static_cast<const BOOL *>(values),count)
                         : dev->SetPixelShaderConstantB(reg,static_cast<const BOOL *>(values),count);
        case 1: return vs ? dev->SetVertexShaderConstantI(reg,static_cast<const int *>(values),count)
                         : dev->SetPixelShaderConstantI(reg,static_cast<const int *>(values),count);
        case 2: return vs ? dev->SetVertexShaderConstantF(reg,static_cast<const float *>(values),count)
                         : dev->SetPixelShaderConstantF(reg,static_cast<const float *>(values),count);
        default: return fail("CTAB setter on sampler " + std::string(d.Name));
        }
    }
    static uint32_t convert(uint32_t bits, UINT from, UINT to) {
        if (from == to) return bits;
        float f; std::memcpy(&f,&bits,4);
        int32_t i; std::memcpy(&i,&bits,4);
        if (to == 1) return from == 3 ? f != 0 : i != 0;
        if (to == 2) {
            if (from != 3) return from == 1 ? !!bits : bits;
            if (!(f>=-2147483648.0f && f<2147483648.0f)) return 0x80000000u;
            return static_cast<int32_t>(f);
        }
        f = from == 1 ? !!bits : static_cast<float>(i);
        std::memcpy(&bits,&f,4); return bits;
    }
    // Input layouts: 0 packed scalars, 1 vectors, 2 matrix, 3 transposed matrix.
    // Walk logical elements, then pack each leaf according to its register set.
    HRESULT assign(IDirect3DDevice9 *dev, Node &n, const uint32_t *in, UINT &left,
                   UINT input_type, UINT layout, UINT &position) {
        const auto &d=n.d;
        UINT logical=d.Rows*d.Columns;
        if (left < logical) { left=0; return S_OK; }
        if (d.Elements > 1) {
            bool first=true;
            for (auto &child : n.children) {
                HRESULT hr;
                if (d.Class==5 && layout==3 && !first && d.RegisterSet!=0 && left>=logical) {
                    std::vector<uint32_t> zero(logical);
                    UINT available=logical, offset=0;
                    hr=assign(dev,*n.children[0],zero.data(),available,input_type,0,offset);
                    left-=logical; position+=logical;
                } else hr=assign(dev,d.Class==5?*n.children[0]:*child,in,left,input_type,layout,position);
                first=false;
                if (FAILED(hr)) return hr;
            }
            return S_OK;
        }
        if (d.Class == 5) {
            if (layout==3 && d.Columns>4)
                return fail("CTAB transposed matrix input to struct exceeding four scalars is unsupported for " + std::string(d.Name),D3DERR_NOTAVAILABLE);
            bool first=true;
            for (auto &child : n.children) {
                // Native D3DX36 treats a normal matrix as packed struct data;
                // transposed input populates the first member, then zeroes the
                // remaining members (FLOAT4/INT4 banks).
                HRESULT hr;
                if (layout==3 && !first && d.RegisterSet!=0) {
                    std::vector<uint32_t> zero(child->d.Bytes/4);
                    UINT available=UINT(zero.size()), offset=0;
                    hr=assign(dev,*child,zero.data(),available,input_type,0,offset);
                    UINT consumed=UINT(zero.size())-available;
                    left=left>consumed?left-consumed:0; position+=consumed;
                } else hr=assign(dev,*child,in,left,input_type,layout==3?3:0,position);
                first=false;
                if (FAILED(hr)) return hr;
            }
            return S_OK;
        }
        if (d.Class == 4) return fail("CTAB numeric setter on object " + std::string(d.Name));
        UINT consumed=logical, stride=d.Columns;
        if (layout == 1) { stride=4; consumed=d.Class==0 ? 1 : d.Rows*4; }
        if (layout >= 2) { stride=4; consumed=d.Class==0 ? (layout==3?4:1) : d.Class==1 ? (layout==3?1:4) : 16; }
        if (left < consumed) { left=0; return S_OK; }
        bool columns=d.Class==3;
        UINT regs=d.RegisterSet==0 ? logical : columns?d.Columns:d.Rows;
        regs=std::min(regs,d.RegisterCount);
        for (UINT r=0;r<regs;++r) {
            uint32_t values[4]={0,0,d.RegisterSet==1?1u:0u,0};
            UINT components=d.RegisterSet==0 ? 1 : columns?d.Rows:d.Columns;
            for (UINT c=0;c<components;++c) {
                UINT row=columns?c:r, col=columns?r:c;
                if (d.RegisterSet==0) { row=r/d.Columns; col=r%d.Columns; }
                UINT src=layout==3 ? col*stride+row : row*stride+col;
                if (src>=left) return fail("CTAB input layout exceeds supplied values for " + std::string(d.Name));
                // Matrix input to scalar/vector walks a column when transposed.
                uint32_t bits;
                std::memcpy(&bits,reinterpret_cast<const uint8_t *>(in)+4*(position+src),4);
                uint32_t v=convert(bits,input_type?input_type:d.Type,d.Type);
                values[c]=convert(v,d.Type,d.RegisterSet==0?1:d.RegisterSet==1?2:3);
            }
            HRESULT hr=write(dev,d,d.RegisterIndex+r,values,1);
            if (FAILED(hr)) return fail("CTAB device register write failed for " + std::string(d.Name),hr);
        }
        position+=consumed; left-=consumed;
        return S_OK;
    }
    HRESULT set(IDirect3DDevice9 *dev,const char *h,const void *p,UINT words,UINT type,UINT layout, bool scalar=false, bool vector=false) {
        diagnostic.clear(); Node *n=node(h);
        if (!dev || !n || (!p && words)) return fail("CTAB setter invalid device, handle or data");
        if (n->d.Class==4) return fail("CTAB numeric setter on sampler " + std::string(n->d.Name));
        if ((scalar && n->d.Class!=0) || (vector && (n->d.Class==2 || n->d.Class==3))) return S_OK;
        UINT position=0;
        return assign(dev,*n,static_cast<const uint32_t *>(p),words,type,layout,position);
    }
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void **out) override {
        if (!out) return E_POINTER;
        *out=nullptr;
        constexpr GUID unknown={0,0,0,{0xc0,0,0,0,0,0,0,0x46}};
        constexpr GUID table={0x9dca3190,0x38b9,0x4fc3,{0x92,0xe3,0x39,0xc6,0xdd,0xfb,0x35,0x8b}};
        if (std::memcmp(&iid,&unknown,sizeof(GUID)) && std::memcmp(&iid,&table,sizeof(GUID))) return E_NOINTERFACE;
        *out=this; AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { ULONG r=--refs; if (!r) delete this; return r; }
    static HRESULT create(const DWORD *shader,size_t words,Table **out) {
        diagnostic.clear(); if (!out) return fail("CTAB null output"); *out=nullptr;
        if (!shader || words<2 || (shader[0]!=D3DVS_VERSION(3,0) && shader[0]!=D3DPS_VERSION(3,0)))
            return fail("CTAB requires bounded shader-model-3 bytecode");
        try {
            auto t=std::unique_ptr<Table>(new Table);
            bool found=false;
            for (size_t i=1;i<words;) {
                DWORD token=shader[i];
                if (token==D3DSIO_END) break;
                if ((token&0xffff)==D3DSIO_COMMENT) {
                    size_t count=(token>>16)&0x7fff;
                    if (count>words-i-1) throw std::runtime_error("CTAB truncated shader comment");
                    if (count && shader[i+1]==0x42415443) {
                        const auto *start=reinterpret_cast<const uint8_t *>(shader+i+2);
                        t->data.assign(start,start+(count-1)*4); found=true; break;
                    }
                    i+=count+1;
                } else { size_t len=(token>>24)&15; if (len>words-i-1) throw std::runtime_error("CTAB truncated shader instruction"); i+=len+1; }
            }
            if (!found) return fail("CTAB comment not found",static_cast<HRESULT>(0x88760b59));
            if (t->read<uint32_t>(0)!=28) throw std::runtime_error("CTAB header size is not 28");
            UINT creator=t->read<uint32_t>(4);
            t->desc={creator?t->string(creator):nullptr,t->read<uint32_t>(8),t->read<uint32_t>(12)};
            UINT info=t->read<uint32_t>(16), target=t->read<uint32_t>(24);
            if (target) t->string(target);
            if (t->desc.Constants>4096) throw std::runtime_error("CTAB constant limit exceeded");
            for (UINT i=0;i<t->desc.Constants;++i) {
                UINT off=info+20*i, def=t->read<uint32_t>(off+16);
                UINT set=t->read<uint16_t>(off+4), index=t->read<uint16_t>(off+6), count=t->read<uint16_t>(off+8);
                auto n=t->parse(t->read<uint32_t>(off+12),t->read<uint32_t>(off),set,index,index+count,def,false,0);
                if (set==1) n->d.RegisterCount=count;
                if (n->d.DefaultValue && (size_t(def) > t->data.size() ||
                    size_t(count)*(set==0?4:16) > t->data.size()-
                        (static_cast<const uint8_t *>(n->d.DefaultValue)-t->data.data())))
                    throw std::runtime_error("CTAB truncated register defaults for " + std::string(n->d.Name));
                t->roots.push_back(std::move(n));
            }
            *out=t.release(); return S_OK;
        } catch (const std::bad_alloc &) { return fail("CTAB allocation failed",E_OUTOFMEMORY); }
          catch (const std::exception &e) { return fail(e.what(),static_cast<HRESULT>(0x88760b59)); }
    }
    HRESULT GetDesc(TableDesc *out) { if (!out) return D3DERR_INVALIDCALL; *out=desc; return S_OK; }
    HRESULT GetConstantDesc(const char *h,ConstantDesc *out,UINT *count) {
        Node *n=node(h); if (!n) return D3DERR_INVALIDCALL;
        if (out) *out=n->d;
        if (count) *count=1;
        return S_OK;
    }
    const char *GetConstant(const char *h,UINT i) {
        Node *n=h?node(h):nullptr;
        if (!h) return i<roots.size()?handle(roots[i].get()):nullptr;
        return n && i<n->d.StructMembers && i<n->children.size()?handle(n->children[i].get()):nullptr;
    }
    const char *GetConstantByName(const char *h,const char *name) { return handle(lookup(h?node(h):nullptr,name)); }
    const char *GetConstantElement(const char *h,UINT i) {
        Node *n=node(h); if (!n || i>=n->d.Elements) return nullptr;
        return handle(n->d.Elements>1?n->children[i].get():n);
    }
    UINT GetSamplerIndex(const char *h) { Node *n=node(h); return n && n->d.RegisterSet==3?n->d.RegisterIndex:UINT_MAX; }
    HRESULT SetDefaults(IDirect3DDevice9 *dev) {
        diagnostic.clear(); if (!dev) return fail("CTAB SetDefaults null device");
        for (auto &n:roots) if (n->d.DefaultValue && n->d.RegisterCount) {
            HRESULT hr=write(dev,n->d,n->d.RegisterIndex,n->d.DefaultValue,n->d.RegisterCount);
            if (FAILED(hr)) return fail("CTAB SetDefaults device write failed for " + std::string(n->d.Name),hr);
        }
        return S_OK;
    }
    HRESULT SetValue(IDirect3DDevice9 *d,const char *h,const void *p,UINT bytes) {
        Node *n=node(h);
        if (!n || (!p && bytes)) return fail("CTAB SetValue invalid handle or null data");
        return set(d,h,p,bytes/4,n->d.Type,0);
    }
    HRESULT SetBool(IDirect3DDevice9 *d,const char *h,BOOL v) { return set(d,h,&v,1,1,0,true); }
    HRESULT SetInt(IDirect3DDevice9 *d,const char *h,INT v) { return set(d,h,&v,1,2,0,true); }
    HRESULT SetFloat(IDirect3DDevice9 *d,const char *h,float v) { return set(d,h,&v,1,3,0,true); }
    HRESULT SetBoolArray(IDirect3DDevice9 *d,const char *h,const BOOL *p,UINT n) { return set(d,h,p,n,1,0); }
    HRESULT SetIntArray(IDirect3DDevice9 *d,const char *h,const INT *p,UINT n) { return set(d,h,p,n,2,0); }
    HRESULT SetFloatArray(IDirect3DDevice9 *d,const char *h,const float *p,UINT n) { return set(d,h,p,n,3,0); }
    HRESULT SetVector(IDirect3DDevice9 *d,const char *h,const Vector *p) { return set(d,h,p,4,3,1,false,true); }
    HRESULT SetVectorArray(IDirect3DDevice9 *d,const char *h,const Vector *p,UINT n) { return n>UINT_MAX/4?D3DERR_INVALIDCALL:set(d,h,p,n*4,3,1); }
    HRESULT SetMatrix(IDirect3DDevice9 *d,const char *h,const Matrix *p) { return set(d,h,p,16,3,2); }
    HRESULT SetMatrixArray(IDirect3DDevice9 *d,const char *h,const Matrix *p,UINT n) { return n>UINT_MAX/16?D3DERR_INVALIDCALL:set(d,h,p,n*16,3,2); }
    HRESULT SetMatrixTranspose(IDirect3DDevice9 *d,const char *h,const Matrix *p) { return set(d,h,p,16,3,3); }
    HRESULT SetMatrixTransposeArray(IDirect3DDevice9 *d,const char *h,const Matrix *p,UINT n) { return n>UINT_MAX/16?D3DERR_INVALIDCALL:set(d,h,p,n*16,3,3); }
};

struct Format {
    D3DFORMAT id;
    int a,r,g,b,kind,bytes,block;
};
// Channel descriptions, not conversion routines. Exclude FOURCC/24-bit fallback
// candidates unless explicitly requested; prefer the source's numeric family.
inline constexpr Format formats[]={
    {D3DFMT_A8R8G8B8,8,8,8,8,0,4,1},{D3DFMT_X8R8G8B8,0,8,8,8,0,4,1},
    {D3DFMT_R8G8B8,0,8,8,8,0,3,1},{D3DFMT_R5G6B5,0,5,6,5,0,2,1},
    {D3DFMT_X1R5G5B5,0,5,5,5,0,2,1},{D3DFMT_A1R5G5B5,1,5,5,5,0,2,1},
    {D3DFMT_A4R4G4B4,4,4,4,4,0,2,1},{D3DFMT_X4R4G4B4,0,4,4,4,0,2,1},
    {D3DFMT_R3G3B2,0,3,3,2,0,1,1},{D3DFMT_A8R3G3B2,8,3,3,2,0,2,1},
    {D3DFMT_A8,8,0,0,0,0,1,1},{D3DFMT_A8B8G8R8,8,8,8,8,0,4,1},
    {D3DFMT_X8B8G8R8,0,8,8,8,0,4,1},{D3DFMT_A2B10G10R10,2,10,10,10,0,4,1},
    {D3DFMT_A2R10G10B10,2,10,10,10,0,4,1},{D3DFMT_G16R16,0,16,16,0,0,4,1},
    {D3DFMT_A16B16G16R16,16,16,16,16,0,8,1},
    {D3DFMT_L8,0,8,0,0,1,1,1},{D3DFMT_A8L8,8,8,0,0,1,2,1},
    {D3DFMT_A4L4,4,4,0,0,1,1,1},{D3DFMT_L16,0,16,0,0,1,2,1},
    {D3DFMT_R16F,0,16,0,0,2,2,1},{D3DFMT_G16R16F,0,16,16,0,2,4,1},
    {D3DFMT_A16B16G16R16F,16,16,16,16,2,8,1},
    {D3DFMT_R32F,0,32,0,0,2,4,1},{D3DFMT_G32R32F,0,32,32,0,2,8,1},
    {D3DFMT_A32B32G32R32F,32,32,32,32,2,16,1},
    {D3DFMT_V8U8,0,8,8,0,3,2,1},{D3DFMT_V16U16,0,16,16,0,3,4,1},
    {D3DFMT_Q8W8V8U8,8,8,8,8,3,4,1},{D3DFMT_Q16W16V16U16,16,16,16,16,3,8,1},
    {D3DFMT_X8L8V8U8,0,8,8,8,3,4,1},{D3DFMT_CxV8U8,0,8,8,0,3,2,1},
    {D3DFMT_D16,0,16,0,0,4,2,1},{D3DFMT_D24X8,0,24,0,0,4,4,1},
    {D3DFMT_D24S8,8,24,0,0,4,4,1},{D3DFMT_D32,0,32,0,0,4,4,1},
    {D3DFMT_DXT1,8,8,8,8,0,0,4},{D3DFMT_DXT2,8,8,8,8,0,0,4},
    {D3DFMT_DXT3,8,8,8,8,0,0,4},{D3DFMT_DXT4,8,8,8,8,0,0,4},
    {D3DFMT_DXT5,8,8,8,8,0,0,4}
};
inline const Format *format_info(D3DFORMAT f) { for (const auto &v:formats) if (v.id==f) return &v; return nullptr; }
inline UINT pow2(UINT v) { UINT p=1; while (p<v && p<0x80000000u) p<<=1; return p; }
inline HRESULT check_texture_requirements(IDirect3DDevice9 *dev,UINT *width,UINT *height,
                                         UINT *levels,DWORD usage,D3DFORMAT *format,D3DPOOL pool) {
    diagnostic.clear();
    if (usage==0xffffffffu) usage=0;
    if (!dev || UINT(pool)>3 || (usage & (D3DUSAGE_WRITEONLY|D3DUSAGE_DONOTCLIP|D3DUSAGE_POINTS|D3DUSAGE_RTPATCHES|D3DUSAGE_NPATCHES)))
        return fail("D3DXCheckTextureRequirements invalid device, usage or pool");
    constexpr DWORD allowed=D3DUSAGE_RENDERTARGET|D3DUSAGE_DEPTHSTENCIL|D3DUSAGE_DYNAMIC|D3DUSAGE_AUTOGENMIPMAP;
    if (usage & ~allowed) return fail("D3DXCheckTextureRequirements unknown usage bits " + std::to_string(usage),D3DERR_NOTAVAILABLE);
    D3DFORMAT chosen=format?*format:D3DFMT_UNKNOWN;
    if (chosen==D3DFMT_UNKNOWN || UINT(chosen)==0xffffffffu) chosen=D3DFMT_A8R8G8B8;
    const Format *info=format_info(chosen);
    if (!info) return fail("D3DXCheckTextureRequirements unknown format " + std::to_string(UINT(chosen)),D3DERR_NOTAVAILABLE);
    D3DCAPS9 caps{}; D3DDEVICE_CREATION_PARAMETERS params{}; D3DDISPLAYMODE mode{};
    HRESULT hr=dev->GetDeviceCaps(&caps); if (FAILED(hr)) return hr;
    hr=dev->GetCreationParameters(&params); if (FAILED(hr)) return hr;
    hr=dev->GetDisplayMode(0,&mode); if (FAILED(hr)) return hr;
    IDirect3D9 *d3d=nullptr; hr=dev->GetDirect3D(&d3d); if (FAILED(hr)) return hr;
    auto supported=[&](D3DFORMAT f) { return SUCCEEDED(d3d->CheckDeviceFormat(params.AdapterOrdinal,params.DeviceType,mode.Format,usage,D3DRTYPE_TEXTURE,f)); };
    if (pool != D3DPOOL_SCRATCH && !supported(chosen)) {
        Format wanted=*info;
        if (wanted.block==4) wanted=*format_info(D3DFMT_A8R8G8B8);
        if (wanted.kind==1) { wanted.kind=0; wanted.g=wanted.b=wanted.r; }
        if (chosen==D3DFMT_CxV8U8) wanted=*format_info(D3DFMT_X8L8V8U8);
        auto channels=[](const Format &f){ return !!f.a+!!f.r+!!f.g+!!f.b; };
        int best=std::numeric_limits<int>::min(); const Format *replacement=nullptr;
        for (const auto &f:formats) {
            // Native D3DX36's fallback families are directional: floating
            // point can fall back to color, while color never selects float,
            // luminance, signed-vector or depth storage as a replacement.
            if (f.kind!=wanted.kind && !(wanted.kind==2 && f.kind==0)) continue;
            if (f.kind==2 && channels(f)!=channels(wanted)) continue;
            Format reference=wanted;
            if (wanted.kind==2 && f.kind==0)
                reference=*format_info(wanted.a?D3DFMT_A8R8G8B8:D3DFMT_X8R8G8B8);
            if (f.block!=1 || f.id==D3DFMT_CxV8U8 || (f.bytes==3 && wanted.bytes!=3) ||
                (reference.a && !f.a) || channels(f)<channels(reference) || !supported(f.id)) continue;
            int score=(f.kind==wanted.kind?10000:0)-100*(channels(f)-channels(reference));
            int bits[]={f.a-reference.a,f.r-reference.r,f.g-reference.g,f.b-reference.b};
            for (int i=0;i<4;++i) score-=(bits[i]<0?-bits[i]*8:bits[i])*(i?2:1);
            if (score>best) { best=score; replacement=&f; }
        }
        if (!replacement) { d3d->Release(); return fail("D3DXCheckTextureRequirements no supported fallback for format " + std::to_string(UINT(chosen)),D3DERR_NOTAVAILABLE); }
        info=replacement; chosen=info->id;
    }
    d3d->Release();
    UINT w=width?std::max(1u,*width):1, h=height?std::max(1u,*height):1;
    if (w==UINT_MAX && h==UINT_MAX) w=h=256;
    else if (w==UINT_MAX) w=height?h:256;
    else if (h==UINT_MAX) h=width?w:256;
    auto dimension=[&](UINT v,UINT maximum) {
        uint64_t rounded=(uint64_t(v)+info->block-1)/info->block*info->block;
        v=UINT(std::min<uint64_t>(rounded,UINT_MAX));
        if (pool==D3DPOOL_SCRATCH) return v;
        bool conditional=(caps.TextureCaps&D3DPTEXTURECAPS_NONPOW2CONDITIONAL) && levels && *levels==1 && info->block==1;
        if ((caps.TextureCaps&D3DPTEXTURECAPS_POW2) && !conditional) v=pow2(v);
        return std::min(v,maximum);
    };
    w=dimension(w,caps.MaxTextureWidth); h=dimension(h,caps.MaxTextureHeight);
    if (width && height && pool!=D3DPOOL_SCRATCH && (caps.TextureCaps&D3DPTEXTURECAPS_SQUAREONLY)) w=h=std::min(w,h);
    if (width) *width=w;
    if (height) *height=h;
    if (levels) {
        UINT maximum=1, size=(!width && !height)?256:std::max(w,h);
        while (size>1) { ++maximum; size>>=1; }
        if (maximum==1 || (pool!=D3DPOOL_SCRATCH && !(caps.TextureCaps&D3DPTEXTURECAPS_MIPMAP))) *levels=1;
        else if (usage&D3DUSAGE_AUTOGENMIPMAP) { if (*levels>1) *levels=0; }
        else if (!*levels || *levels>maximum) *levels=maximum;
    }
    if (format && !(pool==D3DPOOL_SCRATCH && UINT(*format)==UINT_MAX)) *format=chosen;
    return S_OK;
}
} // namespace ac1_d3dx
