// Small Windows-only oracle contract. Compiles original test HLSL in memory;
// never reads game assets, renders, or builds generated game functions.
#define NOMINMAX
#include <windows.h>
#include <d3dx9.h>
#include "../src/graphics/ac1_d3dx_portable.h"
#include <array>
#include <cstdio>
#include <functional>
#include <set>

namespace {
unsigned matches=0, failures=0, descriptors=0, setters=0, textures=0;
unsigned dollar_names=0, truncated_nodes=0, register_sets=0;
void check(bool ok,const std::string &what) {
    if (ok) ++matches;
    else { if (failures<24) std::fprintf(stderr,"FAIL %s\n",what.c_str()); ++failures; }
}
void require(bool ok,const char *s) { if (!ok) throw std::runtime_error(s); }
template<size_t Slot> HRESULT STDMETHODCALLTYPE unexpected_slot() { throw std::runtime_error("unexpected COM slot "+std::to_string(Slot)); }
template<size_t... I> void fill_slots(void **table,std::index_sequence<I...>) { ((table[I]=reinterpret_cast<void *>(&unexpected_slot<I>)),...); }
HMODULE system_dll(const wchar_t *name) {
    wchar_t path[MAX_PATH]; UINT n=GetSystemDirectoryW(path,MAX_PATH);
    require(n && n<MAX_PATH,"GetSystemDirectory");
    std::wstring full(path,n); full+=L"\\"; full+=name;
    HMODULE m=LoadLibraryW(full.c_str()); require(m!=nullptr,"native System32 DLL absent"); return m;
}
template<class T> T symbol(HMODULE dll,const char *name) {
    FARPROC raw=GetProcAddress(dll,name); T typed{};
    static_assert(sizeof(raw)==sizeof(typed)); std::memcpy(&typed,&raw,sizeof(typed));
    require(typed!=nullptr,name); return typed;
}
struct Write {
    unsigned slot,start,count; std::vector<uint32_t> bits;
    bool operator==(const Write &w) const { return slot==w.slot && start==w.start && count==w.count && bits==w.bits; }
};
// Only the six typed Set*ShaderConstant slots are callable. Unexpected native
// calls land in a failing callback; no implementation-only mock expectations.
struct Recorder {
    void **vptr;
    std::array<void *,119> vtable{};
    std::vector<Write> writes;
    HRESULT status=S_OK;
    template<unsigned Slot,unsigned Width>
    static HRESULT STDMETHODCALLTYPE record(Recorder *self,UINT start,const void *p,UINT count) {
        const auto *bits=static_cast<const uint32_t *>(p);
        require(count<4096,"oracle wrote excessive register count");
        for (UINT i=0;i<count;++i)
            self->writes.push_back({Slot,start+i,1,{bits+i*Width,bits+(i+1)*Width}});
        return self->status;
    }
    static HRESULT STDMETHODCALLTYPE unexpected() { throw std::runtime_error("unexpected device method"); }
    Recorder() {
        vptr=vtable.data();
        vtable.fill(reinterpret_cast<void *>(&unexpected));
        vtable[94]=reinterpret_cast<void *>(&record<94,4>);
        vtable[96]=reinterpret_cast<void *>(&record<96,4>);
        vtable[98]=reinterpret_cast<void *>(&record<98,1>);
        vtable[109]=reinterpret_cast<void *>(&record<109,4>);
        vtable[111]=reinterpret_cast<void *>(&record<111,4>);
        vtable[113]=reinterpret_cast<void *>(&record<113,1>);
    }
    IDirect3DDevice9 *device() { return reinterpret_cast<IDirect3DDevice9 *>(this); }
};
struct TextureDevice {
    void **vptr;
    std::array<void *,119> vtable{};
    IDirect3D9 *d3d;
    D3DCAPS9 caps{};
    D3DDISPLAYMODE mode{};
    static HRESULT STDMETHODCALLTYPE query(TextureDevice *self,REFIID iid,void **out) {
        *out=nullptr;
        if (iid.Data1==0 || iid.Data1==0xd0223b96) { *out=self; return S_OK; }
        return E_NOINTERFACE;
    }
    static ULONG STDMETHODCALLTYPE ref(TextureDevice *) { return 1; }
    static HRESULT STDMETHODCALLTYPE get_d3d(TextureDevice *self,IDirect3D9 **out) { *out=self->d3d; (*out)->AddRef(); return S_OK; }
    static HRESULT STDMETHODCALLTYPE get_caps(TextureDevice *self,D3DCAPS9 *out) { *out=self->caps; return S_OK; }
    static HRESULT STDMETHODCALLTYPE get_mode(TextureDevice *self,UINT,D3DDISPLAYMODE *out) { *out=self->mode; return S_OK; }
    static HRESULT STDMETHODCALLTYPE get_params(TextureDevice *,D3DDEVICE_CREATION_PARAMETERS *out) { *out={}; out->DeviceType=D3DDEVTYPE_HAL; return S_OK; }
    explicit TextureDevice(IDirect3D9 *native) : d3d(native) {
        vptr=vtable.data(); fill_slots(vtable.data(),std::make_index_sequence<119>{});
        vtable[0]=reinterpret_cast<void *>(&query); vtable[1]=vtable[2]=reinterpret_cast<void *>(&ref);
        vtable[6]=reinterpret_cast<void *>(&get_d3d); vtable[7]=reinterpret_cast<void *>(&get_caps);
        vtable[8]=reinterpret_cast<void *>(&get_mode); vtable[9]=reinterpret_cast<void *>(&get_params);
        require(SUCCEEDED(d3d->GetDeviceCaps(0,D3DDEVTYPE_HAL,&caps)),"IDirect3D9 caps unavailable");
        require(SUCCEEDED(d3d->GetAdapterDisplayMode(0,&mode)),"adapter display mode unavailable");
    }
    IDirect3DDevice9 *device() { return reinterpret_cast<IDirect3DDevice9 *>(this); }
};
struct FormatProvider {
    void **vptr;
    std::array<void *,17> vtable{};
    D3DCAPS9 caps{};
    unsigned profile=0;
    static HRESULT STDMETHODCALLTYPE query(FormatProvider *self,REFIID iid,void **out) {
        *out=nullptr;
        if (iid.Data1==0 || iid.Data1==0x81bdcbca) { *out=self; return S_OK; }
        return E_NOINTERFACE;
    }
    static ULONG STDMETHODCALLTYPE ref(FormatProvider *) { return 1; }
    static HRESULT STDMETHODCALLTYPE get_caps(FormatProvider *self,UINT,D3DDEVTYPE,D3DCAPS9 *out) { *out=self->caps; return S_OK; }
    static HRESULT STDMETHODCALLTYPE get_mode(FormatProvider *,UINT,D3DDISPLAYMODE *out) { *out={1920,1080,60,D3DFMT_X8R8G8B8}; return S_OK; }
    static HRESULT STDMETHODCALLTYPE check_format(FormatProvider *self,UINT,D3DDEVTYPE,D3DFORMAT,DWORD usage,D3DRESOURCETYPE type,D3DFORMAT f) {
        require(type==D3DRTYPE_TEXTURE,"requirements queried wrong resource type");
        if (self->profile==1) return (f==D3DFMT_A8R8G8B8 || f==D3DFMT_X8R8G8B8)?S_OK:D3DERR_NOTAVAILABLE;
        if (self->profile==2) return D3DERR_NOTAVAILABLE;
        if (self->profile==6) return (f==D3DFMT_R16F || f==D3DFMT_A16B16G16R16F || f==D3DFMT_R32F || f==D3DFMT_A32B32G32R32F)?S_OK:D3DERR_NOTAVAILABLE;
        if (self->profile==7) return (f==D3DFMT_R5G6B5 || f==D3DFMT_A4R4G4B4 || f==D3DFMT_A1R5G5B5 || f==D3DFMT_A8)?S_OK:D3DERR_NOTAVAILABLE;
        if (self->profile==8) return (f==D3DFMT_L8 || f==D3DFMT_A8L8 || f==D3DFMT_A4L4 || f==D3DFMT_L16)?S_OK:D3DERR_NOTAVAILABLE;
        if (self->profile==9) return (f==D3DFMT_Q8W8V8U8 || f==D3DFMT_D16 || f==D3DFMT_D24S8)?S_OK:D3DERR_NOTAVAILABLE;
        if (self->profile==10 && (usage&D3DUSAGE_AUTOGENMIPMAP)) return D3DOK_NOAUTOGEN;
        if (f==D3DFMT_R8G8B8 || f==D3DFMT_L16 || f==D3DFMT_V8U8 || (usage&D3DUSAGE_RENDERTARGET && (f==D3DFMT_DXT1 || f==D3DFMT_DXT5))) return D3DERR_NOTAVAILABLE;
        return ac1_d3dx::format_info(f)?S_OK:D3DERR_NOTAVAILABLE;
    }
    FormatProvider() {
        vptr=vtable.data(); fill_slots(vtable.data(),std::make_index_sequence<17>{});
        vtable[0]=reinterpret_cast<void *>(&query);
        vtable[1]=vtable[2]=reinterpret_cast<void *>(&ref);
        vtable[8]=reinterpret_cast<void *>(&get_mode); vtable[10]=reinterpret_cast<void *>(&check_format);
        vtable[14]=reinterpret_cast<void *>(&get_caps);
        caps.MaxTextureWidth=4096; caps.MaxTextureHeight=2048;
        caps.TextureCaps=D3DPTEXTURECAPS_MIPMAP;
    }
    IDirect3D9 *d3d() { return reinterpret_cast<IDirect3D9 *>(this); }
};
using Compile=HRESULT (WINAPI *)(const char *,UINT,const D3DXMACRO *,ID3DXInclude *,const char *,const char *,DWORD,ID3DXBuffer **,ID3DXBuffer **,ID3DXConstantTable **);
using GetTable=HRESULT (WINAPI *)(const DWORD *,ID3DXConstantTable **);
using Requirements=HRESULT (WINAPI *)(IDirect3DDevice9 *,UINT *,UINT *,UINT *,DWORD,D3DFORMAT *,D3DPOOL);
void shader_contract(Compile compile,GetTable get_table,const char *profile,const char *source) {
    ID3DXBuffer *code=nullptr,*errors=nullptr;
    HRESULT hr=compile(source,UINT(std::strlen(source)),nullptr,nullptr,"main",profile,0,&code,&errors,nullptr);
    if (errors) { if (FAILED(hr)) std::fprintf(stderr,"%s\n",static_cast<char *>(errors->GetBufferPointer())); errors->Release(); }
    require(SUCCEEDED(hr) && code,"D3DXCompileShader failed");
    ID3DXConstantTable *native=nullptr; ac1_d3dx::Table *portable=nullptr;
    require(SUCCEEDED(get_table(static_cast<const DWORD *>(code->GetBufferPointer()),&native)) && native,"native CTAB");
    hr=ac1_d3dx::Table::create(static_cast<const DWORD *>(code->GetBufferPointer()),code->GetBufferSize()/4,&portable);
    require(SUCCEEDED(hr),ac1_d3dx::diagnostic.c_str());
    D3DXCONSTANTTABLE_DESC nd{}; ac1_d3dx::TableDesc pd{};
    native->GetDesc(&nd); portable->GetDesc(&pd);
    check(nd.Version==pd.Version && nd.Constants==pd.Constants && !std::strcmp(nd.Creator,pd.Creator),"GetDesc");
    Recorder a,b;
    auto compare=[&](const std::string &label,const std::function<HRESULT()> &na,const std::function<HRESULT()> &po) {
        a.writes.clear(); b.writes.clear(); HRESULT x=na(),y=po(); ++setters;
        bool ok=x==y && a.writes==b.writes;
        check(ok,std::string(profile)+" "+label+" writes native="+std::to_string(a.writes.size())+" portable="+std::to_string(b.writes.size()));
        if (!ok && failures<24) {
            if (x!=y) std::fprintf(stderr,"  HRESULT native=0x%08lx portable=0x%08lx\n",static_cast<unsigned long>(x),static_cast<unsigned long>(y));
            for (size_t i=0;i<std::min(a.writes.size(),b.writes.size());++i) if (!(a.writes[i]==b.writes[i])) {
                std::fprintf(stderr,"  register %u/%u:",a.writes[i].start,b.writes[i].start);
                for (auto v:a.writes[i].bits) std::fprintf(stderr," %08x",v);
                std::fprintf(stderr," / "); for (auto v:b.writes[i].bits) std::fprintf(stderr," %08x",v);
                std::fprintf(stderr,"\n"); break;
            }
        }
    };
    compare("SetDefaults",[&]{return native->SetDefaults(a.device());},[&]{return portable->SetDefaults(b.device());});
    std::function<void(D3DXHANDLE,const char *,std::string)> visit;
    visit=[&](D3DXHANDLE nh,const char *ph,std::string path) {
        require(nh && ph,"recursive handle absent");
        D3DXCONSTANT_DESC n{}; ac1_d3dx::ConstantDesc p{}; UINT nc=1,pc=1;
        native->GetConstantDesc(nh,&n,&nc); portable->GetConstantDesc(ph,&p,&pc); ++descriptors;
        if (n.Name[0]=='$') ++dollar_names;
        register_sets|=1u<<n.RegisterSet;
        if (n.Class==D3DXPC_MATRIX_COLUMNS && n.RegisterCount<n.Columns*n.Elements) ++truncated_nodes;
        if (n.Class==D3DXPC_MATRIX_ROWS && n.RegisterCount<n.Rows*n.Elements) ++truncated_nodes;
        check(nc==pc && !std::strcmp(n.Name,p.Name) && UINT(n.RegisterSet)==p.RegisterSet &&
            n.RegisterIndex==p.RegisterIndex && n.RegisterCount==p.RegisterCount && UINT(n.Class)==p.Class &&
            UINT(n.Type)==p.Type && n.Rows==p.Rows && n.Columns==p.Columns && n.Elements==p.Elements &&
            n.StructMembers==p.StructMembers && n.Bytes==p.Bytes && !!n.DefaultValue==!!p.DefaultValue &&
            (!n.DefaultValue || !std::memcmp(n.DefaultValue,p.DefaultValue,n.Bytes)),path+" GetConstantDesc");
        if (n.RegisterIndex!=p.RegisterIndex || n.RegisterCount!=p.RegisterCount || n.Bytes!=p.Bytes || n.Rows!=p.Rows || n.Columns!=p.Columns)
            std::fprintf(stderr,"  %s desc native index/count/bytes/rows/cols=%u/%u/%u/%u/%u portable=%u/%u/%u/%u/%u\n",path.c_str(),n.RegisterIndex,n.RegisterCount,n.Bytes,n.Rows,n.Columns,p.RegisterIndex,p.RegisterCount,p.Bytes,p.Rows,p.Columns);
        if (n.DefaultValue && p.DefaultValue && std::memcmp(n.DefaultValue,p.DefaultValue,n.Bytes)) {
            std::fprintf(stderr,"  %s defaults native/portable=%08x/%08x\n",path.c_str(),*static_cast<const unsigned *>(n.DefaultValue),*static_cast<const unsigned *>(p.DefaultValue));
        }
        check(native->GetConstantByName(nullptr,path.c_str())==nh && portable->GetConstantByName(nullptr,path.c_str())==ph,path+" lookup");
        check(native->GetSamplerIndex(nh)==portable->GetSamplerIndex(ph),path+" sampler");
        check(native->GetConstantElement(nh,n.Elements)==nullptr && portable->GetConstantElement(ph,n.Elements)==nullptr,path+" out-of-range element");
        std::array<float,256> values; std::array<INT,256> ints; std::array<BOOL,256> bools;
        for (unsigned i=0;i<values.size();++i) { values[i]=float(int(i%19)-9)+0.25f; ints[i]=int(i%7)-3; bools[i]=(i%3)?-7:0; }
        auto *nv=reinterpret_cast<const D3DXVECTOR4 *>(values.data()); auto *pv=reinterpret_cast<const ac1_d3dx::Vector *>(values.data());
        auto *nm=reinterpret_cast<const D3DXMATRIX *>(values.data()); auto *pm=reinterpret_cast<const ac1_d3dx::Matrix *>(values.data());
        if (n.Class!=D3DXPC_OBJECT) {
            compare(path+" SetFloat",[&]{return native->SetFloat(a.device(),nh,1.25f);},[&]{return portable->SetFloat(b.device(),ph,1.25f);});
            compare(path+" SetInt",[&]{return native->SetInt(a.device(),nh,-3);},[&]{return portable->SetInt(b.device(),ph,-3);});
            compare(path+" SetBool",[&]{return native->SetBool(a.device(),nh,-7);},[&]{return portable->SetBool(b.device(),ph,-7);});
            for (UINT count:{0u,1u,3u,7u,16u,64u}) {
                compare(path+" FloatArray "+std::to_string(count),[&]{return native->SetFloatArray(a.device(),nh,values.data(),count);},[&]{return portable->SetFloatArray(b.device(),ph,values.data(),count);});
                compare(path+" IntArray "+std::to_string(count),[&]{return native->SetIntArray(a.device(),nh,ints.data(),count);},[&]{return portable->SetIntArray(b.device(),ph,ints.data(),count);});
                compare(path+" BoolArray "+std::to_string(count),[&]{return native->SetBoolArray(a.device(),nh,bools.data(),count);},[&]{return portable->SetBoolArray(b.device(),ph,bools.data(),count);});
                compare(path+" SetValue "+std::to_string(count),[&]{return native->SetValue(a.device(),nh,values.data(),count*4);},[&]{return portable->SetValue(b.device(),ph,values.data(),count*4);});
            }
            compare(path+" SetVector",[&]{return native->SetVector(a.device(),nh,nv);},[&]{return portable->SetVector(b.device(),ph,pv);});
            for (UINT count:{0u,1u,2u,5u}) {
                compare(path+" VectorArray "+std::to_string(count),[&]{return native->SetVectorArray(a.device(),nh,nv,count);},[&]{return portable->SetVectorArray(b.device(),ph,pv,count);});
                {
                    compare(path+" MatrixArray "+std::to_string(count),[&]{return native->SetMatrixArray(a.device(),nh,nm,count);},[&]{return portable->SetMatrixArray(b.device(),ph,pm,count);});
                    if (n.Class!=D3DXPC_STRUCT || n.Columns<=4)
                        compare(path+" TransposeArray "+std::to_string(count),[&]{return native->SetMatrixTransposeArray(a.device(),nh,nm,count);},[&]{return portable->SetMatrixTransposeArray(b.device(),ph,pm,count);});
                }
            }
            {
                compare(path+" Matrix",[&]{return native->SetMatrix(a.device(),nh,nm);},[&]{return portable->SetMatrix(b.device(),ph,pm);});
                if (n.Class!=D3DXPC_STRUCT || n.Columns<=4)
                    compare(path+" Transpose",[&]{return native->SetMatrixTranspose(a.device(),nh,nm);},[&]{return portable->SetMatrixTranspose(b.device(),ph,pm);});
                else check(FAILED(portable->SetMatrixTranspose(b.device(),ph,pm)) && !ac1_d3dx::diagnostic.empty(),path+" unsupported matrix-to-large-struct diagnoses");
            }
        }
        if (n.Elements>1) for (UINT i=0;i<n.Elements;++i) visit(native->GetConstantElement(nh,i),portable->GetConstantElement(ph,i),path+"["+std::to_string(i)+"]");
        else for (UINT i=0;i<n.StructMembers;++i) {
            auto ch=native->GetConstant(nh,i); D3DXCONSTANT_DESC cd{}; UINT count=1; native->GetConstantDesc(ch,&cd,&count);
            check(native->GetConstantByName(nh,cd.Name)==ch && portable->GetConstantByName(ph,cd.Name)==portable->GetConstant(ph,i),path+" relative member lookup");
            visit(ch,portable->GetConstant(ph,i),path+"."+cd.Name);
        }
    };
    for (UINT i=0;i<nd.Constants;++i) {
        auto nh=native->GetConstant(nullptr,i); D3DXCONSTANT_DESC cd{}; UINT count=1; native->GetConstantDesc(nh,&cd,&count);
        visit(nh,portable->GetConstant(nullptr,i),cd.Name);
    }
    check(!portable->GetConstantByName(nullptr,"absent[0].x") && !portable->GetConstant(nullptr,nd.Constants),"invalid lookup");
    // Bounded malformed-input paths must fail without reading past the comment.
    std::vector<DWORD> broken(static_cast<const DWORD *>(code->GetBufferPointer()),static_cast<const DWORD *>(code->GetBufferPointer())+code->GetBufferSize()/4);
    ac1_d3dx::Table *bad=nullptr;
    check(FAILED(ac1_d3dx::Table::create(broken.data(),2,&bad)) && !bad,"truncated comment rejected");
    size_t ctab=0, ctab_end=0;
    for (size_t i=1;i<broken.size();++i) if ((broken[i]&0xffff)==D3DSIO_COMMENT && i+1<broken.size() && broken[i+1]==0x42415443) {
        ctab=(i+2)*4; ctab_end=(i+1+((broken[i]>>16)&0x7fff))*4; break;
    }
    require(ctab!=0,"test compiler emitted CTAB");
    auto raw=reinterpret_cast<const uint8_t *>(broken.data());
    auto read32=[&](size_t offset) { uint32_t v; std::memcpy(&v,raw+offset,4); return v; };
    UINT info=read32(ctab+16), type=read32(ctab+info+12);
    for (auto mutation:std::initializer_list<std::array<size_t,2>>{
        {ctab,0},{ctab+4,UINT_MAX},{ctab+16,UINT_MAX},{ctab+info,UINT_MAX},
        {ctab+info+4,99},{ctab+info+12,UINT_MAX},{ctab+type,99},
        {ctab+info+16,ctab_end-ctab-4}}) {
        auto malformed=broken; uint32_t v=UINT(mutation[1]);
        std::memcpy(reinterpret_cast<uint8_t *>(malformed.data())+mutation[0],&v,4);
        check(FAILED(ac1_d3dx::Table::create(malformed.data(),malformed.size(),&bad)) && !bad && !ac1_d3dx::diagnostic.empty(),"malformed CTAB offset/type/default diagnoses");
    }
    if (auto scalar=portable->GetConstantByName(nullptr,"scalar")) {
        compare("SetValue null data",[&]{return native->SetValue(a.device(),native->GetConstantByName(nullptr,"scalar"),nullptr,0);},
            [&]{return portable->SetValue(b.device(),scalar,nullptr,0);});
        b.status=D3DERR_INVALIDCALL;
        check(FAILED(portable->SetFloat(b.device(),scalar,1)) && !ac1_d3dx::diagnostic.empty(),"device setter failure propagates");
        b.status=S_OK;
    }
    native->Release(); portable->Release(); code->Release();
}
void texture_contract(Requirements native) {
    HMODULE module=system_dll(L"d3d9.dll");
    auto create=symbol<IDirect3D9 *(WINAPI *)(UINT)>(module,"Direct3DCreate9");
    require(create!=nullptr,"Direct3DCreate9 export"); IDirect3D9 *d3d=create(D3D_SDK_VERSION); require(d3d!=nullptr,"Direct3DCreate9");
    D3DCAPS9 actual{}; bool have_native_caps=SUCCEEDED(d3d->GetDeviceCaps(0,D3DDEVTYPE_HAL,&actual));
    std::printf("texture oracle: native HAL caps %s; deterministic query-only fixtures always tested\n",have_native_caps?"available":"unavailable");
    FormatProvider provider;
    for (unsigned profile=0;profile<(have_native_caps?12u:11u);++profile) {
    unsigned before_failures=failures, details=0;
    std::set<std::string> detailed;
    provider.profile=profile>=3 && profile<=5?0:profile;
    TextureDevice fixture(profile==11?d3d:provider.d3d()); auto *dev=fixture.device(); auto &caps=fixture.caps;
    if (profile==1) caps.TextureCaps|=D3DPTEXTURECAPS_POW2|D3DPTEXTURECAPS_SQUAREONLY;
    if (profile==3) caps.TextureCaps|=D3DPTEXTURECAPS_POW2|D3DPTEXTURECAPS_NONPOW2CONDITIONAL;
    if (profile==4) caps.TextureCaps|=D3DPTEXTURECAPS_SQUAREONLY;
    if (profile==5) caps.TextureCaps&=~D3DPTEXTURECAPS_MIPMAP;
    for (auto size:std::initializer_list<std::array<UINT,2>>{{0,0},{1,7},{3,5},{255,257},{1023,65},{UINT_MAX,17},{UINT_MAX,UINT_MAX},{caps.MaxTextureWidth+1,caps.MaxTextureHeight+5}})
    for (D3DFORMAT fmt:{D3DFMT_UNKNOWN,D3DFMT_A8R8G8B8,D3DFMT_X8R8G8B8,D3DFMT_R8G8B8,D3DFMT_R5G6B5,D3DFMT_A4R4G4B4,D3DFMT_A8,D3DFMT_L8,D3DFMT_A8L8,D3DFMT_L16,D3DFMT_DXT1,D3DFMT_DXT5,D3DFMT_R16F,D3DFMT_A16B16G16R16F,D3DFMT_R32F,D3DFMT_G32R32F,D3DFMT_V8U8,D3DFMT_D24S8})
    for (UINT levels:{0u,1u,4u,UINT_MAX})
    for (DWORD usage:{DWORD(0),DWORD(D3DUSAGE_RENDERTARGET),DWORD(D3DUSAGE_AUTOGENMIPMAP)})
    for (D3DPOOL pool:{D3DPOOL_DEFAULT,D3DPOOL_MANAGED,D3DPOOL_SYSTEMMEM,D3DPOOL_SCRATCH}) {
        UINT nw=size[0],nh=size[1],nl=levels,pw=nw,ph=nh,pl=nl; D3DFORMAT nf=fmt,pf=fmt;
        HRESULT n=native(dev,&nw,&nh,&nl,usage,&nf,pool),p=ac1_d3dx::check_texture_requirements(dev,&pw,&ph,&pl,usage,&pf,pool); ++textures;
        bool ok=n==p && nw==pw && nh==ph && nl==pl && nf==pf;
        check(ok,"texture fmt="+std::to_string(fmt)+" usage="+std::to_string(usage)+" pool="+std::to_string(pool)+" size="+std::to_string(size[0])+"x"+std::to_string(size[1])+" native="+std::to_string(nf)+" portable="+std::to_string(pf));
        std::string key=std::to_string(fmt)+"/"+std::to_string(usage)+"/"+(nw==pw && nh==ph?"sizeok":"sizebad")+(nl==pl?"mipok":"mipbad")+(nf==pf?"fmtok":"fmtbad");
        if (!ok && detailed.insert(key).second && details++<30) std::printf("profile=%u fmt=%u usage=%lu pool=%u requested=%ux%u levels=%u native=%08lx/%ux%u/%u/%u portable=%08lx/%ux%u/%u/%u\n",profile,fmt,usage,pool,size[0],size[1],levels,static_cast<unsigned long>(n),nw,nh,nl,nf,static_cast<unsigned long>(p),pw,ph,pl,pf);
    }
    std::printf("texture profile %u mismatches=%u\n",profile,failures-before_failures);
    for (unsigned mask=0;mask<16;++mask)
    for (UINT size:{0u,17u,UINT_MAX})
    for (D3DPOOL pool:{D3DPOOL_DEFAULT,D3DPOOL_SCRATCH}) {
        UINT nvalues[]={size,31,UINT_MAX}, pvalues[]={size,31,UINT_MAX};
        D3DFORMAT nf=static_cast<D3DFORMAT>(UINT_MAX),pf=nf;
        HRESULT n=native(dev,mask&1?&nvalues[0]:nullptr,mask&2?&nvalues[1]:nullptr,
            mask&4?&nvalues[2]:nullptr,UINT_MAX,mask&8?&nf:nullptr,pool);
        HRESULT p=ac1_d3dx::check_texture_requirements(dev,mask&1?&pvalues[0]:nullptr,mask&2?&pvalues[1]:nullptr,
            mask&4?&pvalues[2]:nullptr,UINT_MAX,mask&8?&pf:nullptr,pool);
        ++textures; bool ok=n==p && !std::memcmp(nvalues,pvalues,sizeof(nvalues)) && nf==pf;
        check(ok,"texture null/default pointer mask="+std::to_string(mask));
        if (!ok && size==0 && mask==8) std::printf("null profile=%u pool=%u mask=%u native=%08lx/%u,%u,%u/%u portable=%08lx/%u,%u,%u/%u\n",profile,pool,mask,static_cast<unsigned long>(n),nvalues[0],nvalues[1],nvalues[2],nf,static_cast<unsigned long>(p),pvalues[0],pvalues[1],pvalues[2],pf);
    }
    }
    d3d->Release(); FreeLibrary(module);
}
} // namespace
int main() {
    try {
        HMODULE dll=system_dll(L"d3dx9_36.dll");
        auto compile=symbol<Compile>(dll,"D3DXCompileShader");
        auto get_table=symbol<GetTable>(dll,"D3DXGetShaderConstantTable");
        auto requirements=symbol<Requirements>(dll,"D3DXCheckTextureRequirements");
        require(compile && get_table && requirements,"D3DX exports missing");
        const char *source=R"(
            float scalar=2.5; float3 direction; float4 vectors[3];
            row_major float3x4 rm[2]; column_major float4x3 cm[2];
            struct Light { float3 color; float intensity; }; Light light; Light lights[2];
            bool enabled : register(b0); int iterations : register(i0);
            sampler2D tex : register(s3);
            float4 main(float4 pos:TEXCOORD1, float2 uv:TEXCOORD0) : COLOR0 {
                float4 v=pos+scalar+float4(direction,1)+vectors[0]+vectors[1]+vectors[2];
                v.xyz+=mul(rm[0],pos)+mul(rm[1],pos)+mul(pos,cm[0])+mul(pos,cm[1]);
                v.xyz+=light.color*light.intensity+lights[0].color*lights[0].intensity+lights[1].color*lights[1].intensity;
                for (int i=0;i<iterations;++i) v+=scalar;
                if (enabled) v+=tex2Dlod(tex,float4(uv,0,0)); return v;
            })";
        shader_contract(compile,get_table,"ps_3_0",source);
        std::string vs=source; auto where=vs.find(": COLOR0"); vs.replace(where,8,": POSITION");
        shader_contract(compile,get_table,"vs_3_0",vs.c_str());
        const char *partial=R"(
            column_major float4x4 truncated[2];
            float4 initialized[2]={float4(1,2,3,4),float4(5,6,7,8)};
            bool flags[2]:register(b0); int counts:register(i0);
            float4 main(float2 uv:TEXCOORD0, uniform float4 tone):COLOR0 {
                float4 v=initialized[0]+initialized[1]+tone;
                v.x+=truncated[0][0][0]+truncated[1][0][0];
                for (int i=0;i<counts;++i) v*=1.1;
                if (flags[0]) v+=uv.x; if (flags[1]) v+=uv.y; return v;
            })";
        shader_contract(compile,get_table,"ps_3_0",partial);
        const char *mixed=R"(
            struct Mixed { float3 color; int count; bool enabled; row_major float2x3 transform; };
            Mixed mixed[2]; row_major bool2x2 switches:register(b0);
            float4 main(float4 p:TEXCOORD0):COLOR0 {
                float4 v=p;
                if (mixed[0].enabled) v.xyz+=mixed[0].color;
                if (mixed[1].enabled) v.xyz+=mixed[1].color;
                v.xy+=mul(mixed[0].transform,p.xyz)+mul(mixed[1].transform,p.xyz);
                v+=mixed[0].count+mixed[1].count;
                if (switches[0][0]) v.x+=1; if (switches[0][1]) v.y+=1;
                if (switches[1][0]) v.z+=1; if (switches[1][1]) v.w+=1; return v;
            })";
        shader_contract(compile,get_table,"ps_3_0",mixed);
        check(register_sets==15,"BOOL/INT4/FLOAT4/SAMPLER register sets covered");
        check(dollar_names>0,"dollar-prefixed uniform names covered");
        check(truncated_nodes>0,"truncated matrix register counts covered");
        std::printf("shader coverage: register-set mask=%u dollar-names=%u truncated-nodes=%u\n",register_sets,dollar_names,truncated_nodes);
        texture_contract(requirements);
        FreeLibrary(dll);
    } catch (const std::exception &e) { std::fprintf(stderr,"ERROR %s\n",e.what()); return 2; }
    std::printf("native/portable: %u matches, %u mismatches; descriptors=%u setter-cases=%u texture-cases=%u\n",matches,failures,descriptors,setters,textures);
    return failures?1:0;
}
