#include <math.h>
#include "climate.h"
#include "cloud.h"
#include "security_ble.h"
#include "esp_log.h"

static void publish_climate(const climate_data_t *sample)
{
    const security_telemetry_t telemetry = {
        .flags = SECURITY_CLIMATE_ONLY,
        .distance_tenths_cm = UINT16_MAX,
        .light_adc = UINT16_MAX,
        .temperature_tenths_c = sample->temp_valid
            ? lroundf(sample->temp_c * 10.0f) : INT16_MIN,
        .humidity_tenths_pct = sample->hum_valid
            ? lroundf(sample->hum_pct * 10.0f) : UINT16_MAX,
        .pressure_tenths_hpa = sample->press_valid &&
            sample->press_hpa >= 0.0f && sample->press_hpa < 6553.4f
            ? lroundf(sample->press_hpa * 10.0f) : UINT16_MAX,
    };
    security_ble_publish(&telemetry);
    ESP_LOGI("climate_node", "valid T/H/P=%d/%d/%d T=%.1f C H=%.1f %% P=%.1f hPa",
             sample->temp_valid, sample->hum_valid, sample->press_valid,
             sample->temp_c, sample->hum_pct, sample->press_hpa);
}

void app_main(void)
{
    esp_err_t cloud_err = cloud_start(NULL);
    if (cloud_err != ESP_OK) {
        ESP_LOGE("cloud", "Cloud disabled, BLE remains available: %s", esp_err_to_name(cloud_err));
    }
    ESP_LOGI("climate_node", "ESP32-CL, AHT20+BMP280 on SDA=21 SCL=22");
    ESP_ERROR_CHECK(climate_start(publish_climate));
    esp_err_t err = security_ble_start(NULL);
    if (err != ESP_OK) {
        ESP_LOGE("climate_node", "BLE unavailable: %s", esp_err_to_name(err));
    }
}
