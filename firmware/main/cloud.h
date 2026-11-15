#pragma once
#include <stddef.h>
#include <stdint.h>
#include "security_ble.h"

esp_err_t cloud_start(void (*handler)(security_command_t));
void cloud_publish(const uint8_t packet[18], uint8_t rising);
// JSON is accepted only over authenticated BLE. Missing cloud fields keep the existing identity.
esp_err_t cloud_configure(const char *json);
void cloud_status(uint8_t result[4]);
