#include <assert.h>
#include <stdio.h>
#include "../main/sonar_decode.h"

static uint32_t symbol(unsigned duration0, unsigned level0, unsigned duration1, unsigned level1)
{
    return duration0 | (level0 << 15) | (duration1 << 16) | (level1 << 31);
}

int main(void)
{
    uint32_t width = 42;
    uint32_t normal[] = {symbol(200, 0, 640, 1), symbol(30000, 0, 0, 0)};
    assert(sonar_pulse_us(normal, 2, &width) && width == 640); // ~11 cm
    uint32_t first_half[] = {symbol(2000, 1, 30000, 0)};
    assert(sonar_pulse_us(first_half, 1, &width) && width == 2000);
    // Changing the delay before ECHO must not affect the measured pulse.
    for (unsigned delay = 1; delay < 30000; delay += 137) {
        normal[0] = symbol(delay, 0, 640, 1);
        assert(sonar_pulse_us(normal, 2, &width) && width == 640);
    }
    uint32_t missing[] = {symbol(30000, 0, 0, 0)};
    uint32_t stuck[] = {symbol(200, 0, 30000, 1)};
    uint32_t glitch[] = {symbol(200, 0, 10, 1)};
    uint32_t noisy[] = {symbol(200, 0, 640, 1), symbol(50, 0, 900, 1)};
    uint32_t empty[] = {0};
    uint32_t overflow[64] = {0};
    width = 42;
    assert(!sonar_pulse_us(missing, 1, &width));
    assert(!sonar_pulse_us(stuck, 1, &width));
    assert(!sonar_pulse_us(glitch, 1, &width));
    assert(!sonar_pulse_us(noisy, 2, &width));
    assert(!sonar_pulse_us(empty, 1, &width));
    assert(!sonar_pulse_us(overflow, 64, &width));
    assert(!sonar_pulse_us(NULL, 0, &width));
    assert(!sonar_pulse_us(normal, 2, NULL));
    assert(width == 42); // failures must not manufacture a distance
    puts("Sonar decoder tests passed");
    return 0;
}
