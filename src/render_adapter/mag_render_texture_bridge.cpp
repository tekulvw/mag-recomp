/* Located guest reads (see the header). Reached only from cohort captures, which
 * already require the adapter's master switch. */
#include "mag_render_texture_bridge.h"
#include "libs/video/cellGcmSys.h"

extern "C" int32_t mag_render_texture_copy_located(void* opaque, uint32_t location, uint32_t offset, uint32_t bytes,
                                                   uint8_t* out) noexcept {
    try {
        if (!opaque || !out || !bytes || uint64_t(offset) + bytes > (uint64_t(1) << 32)) return -1;
        const auto& source = *static_cast<const MagRenderTextureCopyContext*>(opaque);
        if (!source.copy) return -1;
        uint32_t done = 0;
        while (done < bytes) {
            const uint32_t at = offset + done;
            uint32_t address{}, count{};
            if (cellGcmNativeReadLocatedRange(location, at, bytes - done, &address, &count) || !count
                || count > bytes - done || uint64_t(address) + count > (uint64_t(1) << 32))
                return -1;
            const int32_t status = source.copy(source.opaque, address, count, out + done);
            if (status) return status;
            done += count;
        }
        return 0;
    } catch (...) {
        return -1;
    }
}

extern "C" const uint8_t* mag_render_texture_view_located(void* opaque, uint32_t location, uint32_t offset,
                                                          uint32_t bytes) noexcept {
    try {
        if (!opaque || !bytes || uint64_t(offset) + bytes > (uint64_t(1) << 32)) return nullptr;
        const auto& source = *static_cast<const MagRenderTextureCopyContext*>(opaque);
        if (!source.view) return nullptr;
        uint32_t done = 0, first = 0;
        while (done < bytes) {
            const uint32_t at = offset + done;
            uint32_t address{}, count{};
            if (cellGcmNativeReadLocatedRange(location, at, bytes - done, &address, &count) || !count
                || count > bytes - done || uint64_t(address) + count > (uint64_t(1) << 32))
                return nullptr;
            if (!done) first = address;
            else if (address != first + done) return nullptr;
            done += count;
        }
        return source.view(source.opaque, first, bytes);
    } catch (...) {
        return nullptr;
    }
}
