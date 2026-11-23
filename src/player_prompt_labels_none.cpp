/* Hosts without a keyboard-layout query: prompt labels use the US names. */
#include "player_prompt_labels.h"

extern "C" size_t mag_prompt_host_key_name(uint32_t, char*, size_t) { return 0; }
