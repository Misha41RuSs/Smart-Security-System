#pragma once
#include <stdbool.h>
#include "esp_err.h"

esp_err_t sonar_init(int trigger_gpio, int echo_gpio);
bool sonar_read(float *distance_cm);
