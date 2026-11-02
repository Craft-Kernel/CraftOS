#pragma once
#include <stdint.h>

/* tiny pc-speaker driver, using pit channel 2. */
void speaker_beep(uint32_t freq_hz, uint32_t duration_ms);
void speaker_off(void);