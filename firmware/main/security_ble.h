#pragma once

#include <stdint.h>
#include "esp_err.h"

typedef enum {
    SECURITY_DISARM = 1,
    SECURITY_ARM,
    SECURITY_SIREN_OFF,
    SECURITY_SIREN_ON,
} security_command_t;

enum {
    SECURITY_ARMED = 1 << 0,
    SECURITY_SIREN = 1 << 1,
    SECURITY_TOO_CLOSE = 1 << 2,
    SECURITY_BRIGHT = 1 << 3,
    SECURITY_OVERHEAT = 1 << 4,
    SECURITY_CLIMATE_ONLY = 1 << 5,
};

typedef struct {
    uint8_t flags;
    uint16_t distance_tenths_cm;
    uint16_t light_adc;
    int16_t temperature_tenths_c;
    uint16_t humidity_tenths_pct;
    uint16_t pressure_tenths_hpa;
} security_telemetry_t;

esp_err_t security_ble_start(void (*command_handler)(security_command_t command));
void security_ble_publish(const security_telemetry_t *telemetry);
