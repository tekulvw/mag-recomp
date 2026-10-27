#pragma once
/* Optional controller-cached diagnostic selection; defaults unchanged. */
void mag_jobio_selector_poll();
bool mag_jobio_selector_configured();
bool mag_jobio_selector_begin(void* context,unsigned entry,const char* directory);
bool mag_jobio_selector_end(void* context);
