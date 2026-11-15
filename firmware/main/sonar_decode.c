#include "sonar_decode.h"

bool sonar_pulse_us(const uint32_t *symbols, size_t count, uint32_t *pulse_us)
{
    if (!symbols || !pulse_us || count == 0 || count >= 64) return false;
    uint32_t width = 0;
    unsigned pulses = 0;
    for (size_t i = 0; i < count * 2; i++) {
        const uint32_t half = symbols[i / 2] >> ((i % 2) * 16);
        const uint32_t duration = half & 0x7fff;
        if (!duration) break; // hardware end marker
        if (half & 0x8000) {
            width = duration;
            pulses++;
        }
    }
    if (pulses != 1 || width < 100 || width >= 30000) return false;
    *pulse_us = width;
    return true;
}
