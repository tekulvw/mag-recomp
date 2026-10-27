#pragma once
/* Local default-off PR353 experiment; controller polling only, no hot FS/env. */
int mag_timer_gcm_pump_diagnostics_init();
int mag_timer_gcm_pump_diagnostics_poll_arm();
void mag_timer_gcm_pump_diagnostics_snapshot();
