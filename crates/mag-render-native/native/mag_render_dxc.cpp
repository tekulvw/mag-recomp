#include "mag_render_dxc.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#else
#include <dlfcn.h>
#endif
#include <dxcapi.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

namespace {
template<class T> struct Com {
    T *p=nullptr;
    ~Com() {if(p)p->Release();}
    T **put(){return &p;}
};
struct Library {
#ifdef _WIN32
    HMODULE handle=nullptr;
    ~Library(){if(handle)FreeLibrary(handle);}
#else
    void *handle=nullptr;
    ~Library(){if(handle)dlclose(handle);}
#endif
};
void message(char *out,size_t capacity,const char *bytes,size_t length) {
    if(out && capacity){const size_t n=length<capacity-1?length:capacity-1;std::memcpy(out,bytes,n);out[n]=0;}
}
void message(char *out,size_t capacity,const char *text){message(out,capacity,text,std::strlen(text));}
bool identifier(const char *entry,uint32_t n) {
    if(!entry || !n || n>255)return false;
    for(uint32_t i=0;i<n;++i){const char c=entry[i];
        if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_'||(i && c>='0'&&c<='9')))return false;}
    return true;
}
bool absolute(const char *path) {
    if(!path || !*path)return false;
#ifdef _WIN32
    return (((path[0]>='A'&&path[0]<='Z')||(path[0]>='a'&&path[0]<='z')) &&
        path[1]==':' && (path[2]=='\\'||path[2]=='/')) || (path[0]=='\\'&&path[1]=='\\');
#else
    return path[0]=='/';
#endif
}
void errors(IDxcOperationResult *result,char *out,size_t capacity) {
    Com<IDxcBlobEncoding> blob;
    if(SUCCEEDED(result->GetErrorBuffer(blob.put())) && blob.p)
        message(out,capacity,static_cast<const char *>(blob.p->GetBufferPointer()),blob.p->GetBufferSize());
}
}
extern "C" void mag_render_dxc_free(mag_render_dxc_blob *blob) {
    if(blob){std::free(blob->bytes);*blob={};}
}
extern "C" int mag_render_dxc_compile(const char *library_path,const void *source,
    uint64_t length,const char *entry,uint32_t entry_length,
    mag_render_dxc_blob *out,char *diagnostic,size_t capacity) {
    return mag_render_dxc_compile_stage(library_path,MAG_RENDER_DXC_COMPUTE,source,
        length,entry,entry_length,out,diagnostic,capacity);
}
extern "C" int mag_render_dxc_compile_stage(const char *library_path,uint32_t stage,
    const void *source,uint64_t length,const char *entry,uint32_t entry_length,
    mag_render_dxc_blob *out,char *diagnostic,size_t capacity) {
    if(out)*out={};
    if(diagnostic && capacity)diagnostic[0]=0;
    const wchar_t *target=nullptr;
    switch(stage){
        case MAG_RENDER_DXC_COMPUTE:target=L"cs_6_0";break;
        case MAG_RENDER_DXC_VERTEX:target=L"vs_6_0";break;
        case MAG_RENDER_DXC_FRAGMENT:target=L"ps_6_0";break;
        default:return MAG_RENDER_DXC_INVALID;
    }
    if(!out || !absolute(library_path) || !source || !length || length>64u*1024u*1024u ||
       !identifier(entry,entry_length))return MAG_RENDER_DXC_INVALID;
    try {
        Library library;
#ifdef _WIN32
        const int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,library_path,-1,nullptr,0);
        if(!n)return MAG_RENDER_DXC_INVALID;
        std::vector<wchar_t> path(static_cast<size_t>(n));
        if(!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,library_path,-1,path.data(),n))return MAG_RENDER_DXC_INVALID;
        library.handle=LoadLibraryExW(path.data(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
        const FARPROC symbol=library.handle?GetProcAddress(library.handle,"DxcCreateInstance"):nullptr;
        DxcCreateInstanceProc create=nullptr;
        static_assert(sizeof(create)==sizeof(symbol),"Windows function pointer size");
        std::memcpy(&create,&symbol,sizeof(create));
#else
        library.handle=dlopen(library_path,RTLD_NOW|RTLD_LOCAL);
        auto create=library.handle?reinterpret_cast<DxcCreateInstanceProc>(dlsym(library.handle,"DxcCreateInstance")):nullptr;
#endif
        if(!create){message(diagnostic,capacity,"Explicit DXC library unavailable or missing DxcCreateInstance");return MAG_RENDER_DXC_UNAVAILABLE;}
        Com<IDxcCompiler3> compiler;
        if(FAILED(create(CLSID_DxcCompiler,__uuidof(IDxcCompiler3),reinterpret_cast<void **>(compiler.put()))) || !compiler.p){
            message(diagnostic,capacity,"DXC Compiler3 unavailable");return MAG_RENDER_DXC_UNAVAILABLE;}
        std::wstring wide_entry;for(uint32_t i=0;i<entry_length;++i)wide_entry.push_back(static_cast<wchar_t>(entry[i]));
        // Warnings stay errors, except Naga's redundant parentheses around
        // equality conditions (if ((a == b))), which are its normal output.
        // HLSL 2018 semantics: the owned programs come from the NV40 program
        // decompilers, whose output is legacy HLSL (the dialect FXC and the
        // glslang HLSL front end on Metal/Vulkan accept), including per-component
        // `cond ? a : b` on vectors. HLSL 2021 rejects a vector condition there
        // (it requires select()), so the same program would compile on Metal and
        // Vulkan but not on D3D12. Naga's HLSL is valid in both versions.
        const wchar_t *args[]={L"-E",wide_entry.c_str(),L"-T",target,L"-HV",L"2018",L"-Ges",L"-WX",
            L"-Wno-parentheses-equality"};
        DxcBuffer buffer={source,static_cast<size_t>(length),DXC_CP_UTF8};
        Com<IDxcResult> result;
        if(FAILED(compiler.p->Compile(&buffer,args,static_cast<uint32_t>(sizeof(args)/sizeof(*args)),nullptr,
            __uuidof(IDxcResult),reinterpret_cast<void **>(result.put()))) || !result.p){
            message(diagnostic,capacity,"DXC Compile API failed");return MAG_RENDER_DXC_INVALID;}
        HRESULT status=E_FAIL;
        if(FAILED(result.p->GetStatus(&status)) || FAILED(status)){errors(result.p,diagnostic,capacity);return MAG_RENDER_DXC_INVALID;}
        Com<IDxcBlob> object;
        if(FAILED(result.p->GetResult(object.put())) || !object.p || !object.p->GetBufferSize()){
            message(diagnostic,capacity,"DXC returned no DXIL object");return MAG_RENDER_DXC_INVALID;}
        Com<IDxcValidator> validator;Com<IDxcOperationResult> validation;
        if(FAILED(create(CLSID_DxcValidator,__uuidof(IDxcValidator),reinterpret_cast<void **>(validator.put()))) || !validator.p ||
           FAILED(validator.p->Validate(object.p,DxcValidatorFlags_Default,validation.put())) || !validation.p){
            message(diagnostic,capacity,"DXIL validation API unavailable or failed");return MAG_RENDER_DXC_UNAVAILABLE;}
        if(FAILED(validation.p->GetStatus(&status)) || FAILED(status)){errors(validation.p,diagnostic,capacity);return MAG_RENDER_DXC_INVALID;}
        const size_t size=object.p->GetBufferSize();
        void *copy=std::malloc(size);if(!copy)return MAG_RENDER_DXC_ALLOCATION_FAILED;
        std::memcpy(copy,object.p->GetBufferPointer(),size);*out={copy,static_cast<uint64_t>(size)};
        return MAG_RENDER_DXC_OK;
    }catch(const std::bad_alloc &){return MAG_RENDER_DXC_ALLOCATION_FAILED;}
}
