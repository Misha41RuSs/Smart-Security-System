#include "climate.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_log.h"

// --- I2C: модуль AHT20 (+ BMP280) ---
#define I2C_SDA_GPIO GPIO_NUM_21
#define I2C_SCL_GPIO GPIO_NUM_22
#define AHT20_ADDR 0x38
#define BMP280_ADDR_PRIMARY 0x77
#define BMP280_ADDR_SECONDARY 0x76
#define I2C_TIMEOUT_MS 100
#define CLIMATE_PERIOD_MS 2000

static const char *TAG = "climate";
static void (*on_sample)(const climate_data_t *);

/* ======================= AHT20 (температура + влажность) ======================= */

/* CRC-8: полином x^8 + x^5 + x^4 + 1 (0x31), init 0xFF */
static uint8_t aht20_crc8(const uint8_t *data, int len)
{
    uint8_t crc = 0xFF;
    for (int i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
    }
    return crc;
}

static esp_err_t aht20_init(i2c_master_dev_handle_t dev)
{
    vTaskDelay(pdMS_TO_TICKS(40));               /* время старта после питания */

    uint8_t cmd = 0x71, status = 0;
    esp_err_t err = i2c_master_transmit_receive(dev, &cmd, 1, &status, 1, I2C_TIMEOUT_MS);
    if (err != ESP_OK) return err;

    if (!(status & 0x08)) {                       /* не откалиброван */
        uint8_t init_cmd[3] = {0xBE, 0x08, 0x00};
        err = i2c_master_transmit(dev, init_cmd, sizeof(init_cmd), I2C_TIMEOUT_MS);
        if (err != ESP_OK) return err;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return ESP_OK;
}

static esp_err_t aht20_read(i2c_master_dev_handle_t dev, float *temp, float *hum)
{
    uint8_t trig[3] = {0xAC, 0x33, 0x00};
    esp_err_t err = i2c_master_transmit(dev, trig, sizeof(trig), I2C_TIMEOUT_MS);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(80));

    uint8_t d[7];
    for (int tries = 0; ; tries++) {
        err = i2c_master_receive(dev, d, sizeof(d), I2C_TIMEOUT_MS);
        if (err != ESP_OK) return err;
        if (!(d[0] & 0x80)) break;                /* bit7 = busy */
        if (tries >= 5) return ESP_ERR_TIMEOUT;
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (aht20_crc8(d, 6) != d[6]) return ESP_ERR_INVALID_CRC;

    uint32_t raw_h = ((uint32_t)d[1] << 12) | ((uint32_t)d[2] << 4) | (d[3] >> 4);
    uint32_t raw_t = ((uint32_t)(d[3] & 0x0F) << 16) | ((uint32_t)d[4] << 8) | d[5];

    *hum  = raw_h * 100.0f / 1048576.0f;          /* 2^20 */
    *temp = raw_t * 200.0f / 1048576.0f - 50.0f;
    return ESP_OK;
}

/* ======================= BMP280 (давление + температура) ======================= */

typedef struct {
    uint16_t T1; int16_t T2, T3;
    uint16_t P1; int16_t P2, P3, P4, P5, P6, P7, P8, P9;
} bmp280_calib_t;

static esp_err_t bmp280_read_regs(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(dev, &reg, 1, buf, len, I2C_TIMEOUT_MS);
}

static esp_err_t bmp280_write_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
}

static esp_err_t bmp280_init(i2c_master_dev_handle_t dev, bmp280_calib_t *cal)
{
    uint8_t id = 0;
    esp_err_t err = bmp280_read_regs(dev, 0xD0, &id, 1);
    if (err != ESP_OK) return err;
    if (id != 0x58 && id != 0x60) {               /* 0x58 = BMP280, 0x60 = BME280 */
        ESP_LOGW(TAG, "Unknown chip id 0x%02X at BMP280 address", id);
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint8_t c[24];
    err = bmp280_read_regs(dev, 0x88, c, sizeof(c));
    if (err != ESP_OK) return err;
    cal->T1 = (uint16_t)(c[1] << 8 | c[0]);
    cal->T2 = (int16_t)(c[3] << 8 | c[2]);
    cal->T3 = (int16_t)(c[5] << 8 | c[4]);
    cal->P1 = (uint16_t)(c[7] << 8 | c[6]);
    cal->P2 = (int16_t)(c[9] << 8 | c[8]);
    cal->P3 = (int16_t)(c[11] << 8 | c[10]);
    cal->P4 = (int16_t)(c[13] << 8 | c[12]);
    cal->P5 = (int16_t)(c[15] << 8 | c[14]);
    cal->P6 = (int16_t)(c[17] << 8 | c[16]);
    cal->P7 = (int16_t)(c[19] << 8 | c[18]);
    cal->P8 = (int16_t)(c[21] << 8 | c[20]);
    cal->P9 = (int16_t)(c[23] << 8 | c[22]);

    /* config: standby 1000 мс, фильтр x4; ctrl_meas: T x2, P x16, normal mode */
    err = bmp280_write_reg(dev, 0xF5, 0xA8);
    if (err != ESP_OK) return err;
    return bmp280_write_reg(dev, 0xF4, 0x57);
}

/* Формулы компенсации из даташита Bosch BMP280 (раздел 8.2) */
static esp_err_t bmp280_read(i2c_master_dev_handle_t dev, const bmp280_calib_t *cal,
                             float *temp, float *press_hpa)
{
    uint8_t d[6];
    esp_err_t err = bmp280_read_regs(dev, 0xF7, d, sizeof(d));
    if (err != ESP_OK) return err;

    int32_t adc_P = (int32_t)((uint32_t)d[0] << 12 | (uint32_t)d[1] << 4 | d[2] >> 4);
    int32_t adc_T = (int32_t)((uint32_t)d[3] << 12 | (uint32_t)d[4] << 4 | d[5] >> 4);
    if (adc_T == 0x80000 || adc_P == 0x80000) return ESP_ERR_INVALID_STATE; /* ещё нет измерения */

    int32_t var1 = ((((adc_T >> 3) - ((int32_t)cal->T1 << 1))) * (int32_t)cal->T2) >> 11;
    int32_t var2 = (((((adc_T >> 4) - (int32_t)cal->T1) * ((adc_T >> 4) - (int32_t)cal->T1)) >> 12)
                    * (int32_t)cal->T3) >> 14;
    int32_t t_fine = var1 + var2;
    *temp = ((t_fine * 5 + 128) >> 8) / 100.0f;

    int64_t p1 = (int64_t)t_fine - 128000;
    int64_t p2 = p1 * p1 * (int64_t)cal->P6;
    p2 = p2 + ((p1 * (int64_t)cal->P5) << 17);
    p2 = p2 + ((int64_t)cal->P4 << 35);
    p1 = ((p1 * p1 * (int64_t)cal->P3) >> 8) + ((p1 * (int64_t)cal->P2) << 12);
    p1 = ((((int64_t)1) << 47) + p1) * (int64_t)cal->P1 >> 33;
    if (p1 == 0) return ESP_ERR_INVALID_STATE;    /* защита от деления на ноль */

    int64_t p = 1048576 - adc_P;
    p = (((p << 31) - p2) * 3125) / p1;
    p1 = ((int64_t)cal->P9 * (p >> 13) * (p >> 13)) >> 25;
    p2 = ((int64_t)cal->P8 * p) >> 19;
    p = ((p + p1 + p2) >> 8) + ((int64_t)cal->P7 << 4);  /* Па в формате Q24.8 */

    *press_hpa = (float)p / 256.0f / 100.0f;
    return ESP_OK;
}

/* ======================= Задача опроса климата ======================= */

static void climate_task(void *arg)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA_GPIO,
        .scl_io_num = I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    if (i2c_new_master_bus(&bus_cfg, &bus) != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed, climate sensors disabled");
        vTaskDelete(NULL);
        return;
    }

    i2c_master_dev_handle_t aht = NULL, bmp = NULL;
    bool aht_ready = false, bmp_ready = false;
    bmp280_calib_t bmp_cal;
    int cycle = 0;

    while (1) {
        /* Подключение / переподключение датчиков (можно воткнуть модуль на ходу).
           Отсутствующий BMP280 ищем реже, чтобы не засорять шину. */
        if (!aht_ready) {
            if (aht == NULL && i2c_master_probe(bus, AHT20_ADDR, 50) == ESP_OK) {
                i2c_device_config_t cfg = {
                    .dev_addr_length = I2C_ADDR_BIT_LEN_7,
                    .device_address = AHT20_ADDR,
                    .scl_speed_hz = 100000,
                };
                i2c_master_bus_add_device(bus, &cfg, &aht);
            }
            if (aht != NULL && aht20_init(aht) == ESP_OK) {
                aht_ready = true;
                ESP_LOGI(TAG, "AHT20 ready");
            }
        }
        if (!bmp_ready && cycle % 10 == 0) {
            if (bmp == NULL) {
                uint16_t addr = 0;
                if (i2c_master_probe(bus, BMP280_ADDR_PRIMARY, 50) == ESP_OK) addr = BMP280_ADDR_PRIMARY;
                else if (i2c_master_probe(bus, BMP280_ADDR_SECONDARY, 50) == ESP_OK) addr = BMP280_ADDR_SECONDARY;
                if (addr) {
                    i2c_device_config_t cfg = {
                        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
                        .device_address = addr,
                        .scl_speed_hz = 100000,
                    };
                    i2c_master_bus_add_device(bus, &cfg, &bmp);
                }
            }
            if (bmp != NULL && bmp280_init(bmp, &bmp_cal) == ESP_OK) {
                bmp_ready = true;
                ESP_LOGI(TAG, "BMP280 ready");
            }
        }

        /* Чтение */
        climate_data_t snap = {0};
        float t, h;
        if (aht_ready) {
            if (aht20_read(aht, &t, &h) == ESP_OK) {
                snap.temp_valid = snap.hum_valid = true;
                snap.temp_c = t;
                snap.hum_pct = h;
            } else {
                aht_ready = false;
                ESP_LOGW(TAG, "AHT20 read failed, will reinit");
            }
        }
        if (bmp_ready) {
            float bt, p;
            esp_err_t err = bmp280_read(bmp, &bmp_cal, &bt, &p);
            if (err == ESP_OK) {
                snap.press_valid = true;
                snap.press_hpa = p;
                if (!snap.temp_valid) {           /* нет AHT20 — берём температуру с BMP280 */
                    snap.temp_valid = true;
                    snap.temp_c = bt;
                }
            } else if (err != ESP_ERR_INVALID_STATE) {
                bmp_ready = false;
                ESP_LOGW(TAG, "BMP280 read failed, will reinit");
            }
        }

        on_sample(&snap);

        cycle++;
        vTaskDelay(pdMS_TO_TICKS(CLIMATE_PERIOD_MS));
    }
}


esp_err_t climate_start(void (*callback)(const climate_data_t *))
{
    if (callback == NULL) return ESP_ERR_INVALID_ARG;
    on_sample = callback;
    return xTaskCreate(climate_task, "climate_task", 4096, NULL, 4, NULL) == pdPASS
        ? ESP_OK : ESP_ERR_NO_MEM;
}
