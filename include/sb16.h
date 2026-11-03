#pragma once
#include <stdint.h>

/* minimal sound blaster 16 driver (pcm playback via isa dma). */
int sb16_init(void);
void sb16_play_tone(uint32_t freq_hz, uint32_t duration_ms);
void sb16_chime_text(void);
void sb16_chime_ui(void);
void sb16_chime_dialog(void);
