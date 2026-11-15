#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// RMT symbols at 1 MHz: duration0:15, level0:1, duration1:15, level1:1.
// Accept one echo pulse; absent, stuck-high and noisy multi-pulse frames fail.
bool sonar_pulse_us(const uint32_t *symbols, size_t count, uint32_t *pulse_us);
