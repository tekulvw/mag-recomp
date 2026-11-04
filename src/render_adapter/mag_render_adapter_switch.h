#pragma once
/* The render adapter's single runtime master switch, shared by its Rust crate
 * and every C++ hook bridge. It is off unless the process sets
 * MAG_RENDER_ADAPTER=1 or calls mag_render_adapter_enable() before its first
 * frame; once on it stays on. Every bridge entry point that a lifted-source
 * hook reaches checks it first and returns at once while it is off, so a
 * prepared lift with the switch off does no adapter work at all. */
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
uint32_t mag_render_adapter_enabled(void);
void mag_render_adapter_enable(void);
#ifdef __cplusplus
}
inline bool mag_render_hooks_enabled() noexcept { return mag_render_adapter_enabled() != 0; }
#endif
