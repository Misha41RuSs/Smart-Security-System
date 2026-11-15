#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    bool temp_valid;
    bool hum_valid;
    bool press_valid;
    float temp_c;
    float hum_pct;
    float press_hpa;
} climate_data_t;

// The callback runs in the climate task after each sample, including failures.
esp_err_t climate_start(void (*callback)(const climate_data_t *));
