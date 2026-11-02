#include "mag_render_host_bridge.h"
#include <cstddef>
#include <cstring>
#if defined(__APPLE__)
#include "rsx_metal_backend.h"
static_assert(sizeof(MagRenderHostOwner)==sizeof(rsx_native_owner));
static_assert(alignof(MagRenderHostOwner)==alignof(rsx_native_owner));
static_assert(offsetof(MagRenderHostOwner,lifetime)==offsetof(rsx_native_owner,lifetime));
static_assert(offsetof(MagRenderHostOwner,retain)==offsetof(rsx_native_owner,retain));
static_assert(offsetof(MagRenderHostOwner,try_begin)==offsetof(rsx_native_owner,try_begin));
static_assert(sizeof(MagRenderHostImage)==sizeof(rsx_native_image));
static_assert(alignof(MagRenderHostImage)==alignof(rsx_native_image));
static_assert(offsetof(MagRenderHostImage,resource_generation)==offsetof(rsx_native_image,resource_generation));
static_assert(offsetof(MagRenderHostImage,image)==offsetof(rsx_native_image,image));
static_assert(offsetof(MagRenderHostImage,retain)==offsetof(rsx_native_image,retain));
static_assert(offsetof(MagRenderHostImage,validate)==offsetof(rsx_native_image,validate));
#endif
extern "C" int mag_render_frontend_borrow_owner(MagRenderHostOwner *out){
 if(!out||out->abi_version!=MAG_RENDER_HOST_OWNER_ABI||out->struct_size!=sizeof(*out))return -1;
 if(mag_render_native_abi_version()!=1)return -2;
#if defined(__APPLE__)
 rsx_native_owner owner={};owner.abi_version=1;owner.struct_size=sizeof(owner);
 int status=rsx_metal_backend_borrow_owner(&owner);if(!status)std::memcpy(out,&owner,sizeof(owner));return status;
#else
 return -2;
#endif
}
extern "C" int mag_render_frontend_borrow_image(uint64_t life,uint32_t surface,MagRenderHostImage *out){
 if(!out||out->abi_version!=MAG_RENDER_HOST_IMAGE_ABI||out->struct_size!=sizeof(*out))return -1;
#if defined(__APPLE__)
 rsx_native_image image={};image.abi_version=1;image.struct_size=sizeof(image);
 int status=rsx_metal_backend_borrow_image(life,surface,&image);if(!status)std::memcpy(out,&image,sizeof(image));return status;
#else
 (void)life;(void)surface;return -2;
#endif
}
