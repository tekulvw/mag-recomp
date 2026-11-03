/* Constructor negative paths: poisoned native handles prove retain-before-use.
 * No device, queue, window, or GPU is created by this fixture. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../../native/mag_render_native.h"
extern "C" {
#include "../../native/mag_render_native_internal.h"
}
#include <cassert>
#include <cstdio>
#include <initializer_list>
#include <string>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static int retains, releases, answer;
static int retain(void *, uint64_t life) {
  assert(life == 7);
  ++retains;
  return answer;
}
static void release(void *, uint64_t life) {
  assert(life == 7);
  ++releases;
}
static int begin(void *, uint64_t) {
  assert(!"constructor must not submit work");
  return -6;
}
static void end(void *, uint64_t) {
  assert(!"constructor must not submit work");
}
extern "C" mag_native_device *mag_native_wrap(const mag_native_ops *, void *) {
  assert(!"invalid constructor must never wrap a device");
  return nullptr;
}
extern "C" uint64_t mag_native_next_generation(void) { return 1; }
int main() {
  MagRenderHostOwner owner = {};
  owner.abi_version = 1;
  owner.struct_size = sizeof(owner);
  owner.backend = MAG_RENDER_HOST_D3D12;
  owner.lifetime = 7;
  owner.device = reinterpret_cast<void *>(uintptr_t(1));
  owner.queue = reinterpret_cast<void *>(uintptr_t(2));
  owner.retain = retain;
  owner.release = release;
  owner.try_begin = begin;
  owner.end = end;
  for (int status : {-3, -4, -6}) {
    answer = status;
    mag_native_device *device =
        reinterpret_cast<mag_native_device *>(uintptr_t(1));
    assert(mag_native_d3d12_create(&owner, &device) == status);
    assert(!device);
  }
  assert(retains == 3 && releases == 0);
  owner.abi_version = 2;
  mag_native_device *device = nullptr;
  assert(mag_native_d3d12_create(&owner, &device) == MAG_NATIVE_INVALID);
  assert(retains == 3);
  owner.abi_version = 1;
  owner.release = nullptr;
  assert(mag_native_d3d12_create(&owner, &device) == MAG_NATIVE_INVALID);
  assert(retains == 3);
  owner.release = release;
  // Even successful retention must be balanced when compiler configuration
  // rejects before dereferencing the poisoned device/queue handles.
  const DWORD count =
      GetEnvironmentVariableW(L"PS3RECOMP_DXC_LIBRARY", nullptr, 0);
  std::wstring saved(count, L'\0');
  if (count) {
    const DWORD copied =
        GetEnvironmentVariableW(L"PS3RECOMP_DXC_LIBRARY", saved.data(), count);
    assert(copied && copied < count);
    saved.resize(copied);
  }
  answer = 0;
  assert(SetEnvironmentVariableW(L"PS3RECOMP_DXC_LIBRARY", nullptr));
  assert(mag_native_d3d12_create(&owner, &device) == MAG_NATIVE_UNSUPPORTED);
  assert(!device && retains == 4 && releases == 1);
  assert(SetEnvironmentVariableW(L"PS3RECOMP_DXC_LIBRARY",
                                 L"relative\\dxcompiler.dll"));
  assert(mag_native_d3d12_create(&owner, &device) == MAG_NATIVE_INVALID);
  assert(!device && retains == 5 && releases == 2);
  assert(SetEnvironmentVariableW(L"PS3RECOMP_DXC_LIBRARY",
                                 count ? saved.c_str() : nullptr));
  std::puts(
      "D3D12 constructor: rejected owner never dereferences poisoned handles");
}
