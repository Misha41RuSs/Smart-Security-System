#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/uart.h"
#include "driver/i2c_master.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "security_ble.h"

#define EX_UART_NUM UART_NUM_0
#define BUF_SIZE (1024)

// --- Пины датчиков ---
#define TRIG_GPIO 5
#define ECHO_GPIO 18
#define LIGHT_SENSOR_CHAN ADC_CHANNEL_6  // GPIO34

// --- I2C: модуль AHT20 (+ BMP280) ---
#define I2C_SDA_GPIO GPIO_NUM_21
#define I2C_SCL_GPIO GPIO_NUM_22
#define AHT20_ADDR 0x38
#define BMP280_ADDR_PRIMARY 0x77
#define BMP280_ADDR_SECONDARY 0x76
#define I2C_TIMEOUT_MS 100
#define CLIMATE_PERIOD_MS 2000

// --- Пины индикации и кнопок ---
#define LED_GREEN_GPIO 25       // Внешний Зеленый (Перегрев: температура > порога)
#define LED_RED_GPIO 26         // Внешний Красный (Дистанция < порога)
#define BOOT_BTN_GPIO 0         // Встроенная кнопка BOOT
#define BUILTIN_LED_GPIO 2      // Встроенный синий светодиод на плате

// --- Настройки зуммера ---
#define BUZZER_GPIO 17
#define LEDC_FREQUENCY 800
#define LEDC_DUTY_QUIET 3

static const char *TAG = "climate";

typedef struct {
    bool armed;
    bool siren;
    float dist_threshold;
    int light_threshold;
    float temp_threshold;
} security_settings_t;

static portMUX_TYPE settings_lock = portMUX_INITIALIZER_UNLOCKED;
static security_settings_t settings = {
    .dist_threshold = 5.0f,
    .light_threshold = 800,
    .temp_threshold = 40.0f,
};

static void apply_security_command(security_command_t command)
{
    taskENTER_CRITICAL(&settings_lock);
    switch (command) {
    case SECURITY_DISARM: settings.armed = false; break;
    case SECURITY_ARM: settings.armed = true; break;
    case SECURITY_SIREN_OFF: settings.siren = false; break;
    case SECURITY_SIREN_ON: settings.siren = true; break;
    }
    taskEXIT_CRITICAL(&settings_lock);
}

// Последние показания климатических датчиков (пишет climate_task, читает sensor_task)
typedef struct {
    bool temp_valid;
    bool hum_valid;
    bool press_valid;
    float temp_c;
    float hum_pct;
    float press_hpa;
} climate_data_t;

static climate_data_t climate = {0};
static portMUX_TYPE climate_lock = portMUX_INITIALIZER_UNLOCKED;

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

        taskENTER_CRITICAL(&climate_lock);
        climate = snap;
        taskEXIT_CRITICAL(&climate_lock);

        cycle++;
        vTaskDelay(pdMS_TO_TICKS(CLIMATE_PERIOD_MS));
    }
}

/* ======================= Зуммер ======================= */

static void buzzer_init(void) {
    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_13_BIT,
        .freq_hz = LEDC_FREQUENCY,
        .clk_cfg = LEDC_AUTO_CLK
    };
    ledc_timer_config(&ledc_timer);

    ledc_channel_config_t ledc_channel = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .gpio_num = BUZZER_GPIO,
        .duty = 0,
        .hpoint = 0
    };
    ledc_channel_config(&ledc_channel);
}

/* ======================= Основной цикл датчиков ======================= */

static bool read_distance(float *distance_cm)
{
    if (gpio_get_level(ECHO_GPIO) != 0) return false;
    gpio_set_level(TRIG_GPIO, 0);
    esp_rom_delay_us(2);
    gpio_set_level(TRIG_GPIO, 1);
    esp_rom_delay_us(10);
    gpio_set_level(TRIG_GPIO, 0);

    int64_t start = esp_timer_get_time();
    while (gpio_get_level(ECHO_GPIO) == 0) {
        if (esp_timer_get_time() - start >= 30000) return false;
    }
    int64_t echo_start = esp_timer_get_time();
    while (gpio_get_level(ECHO_GPIO) == 1) {
        if (esp_timer_get_time() - echo_start >= 30000) return false;
    }
    int64_t pulse_us = esp_timer_get_time() - echo_start;
    if (pulse_us <= 0 || pulse_us >= 30000) return false;
    *distance_cm = pulse_us * 0.0343f / 2.0f;
    return true;
}

static void sensor_task(void *arg)
{
    // Инициализация датчиков
    gpio_set_direction(TRIG_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_direction(ECHO_GPIO, GPIO_MODE_INPUT);

    // Инициализация светодиодов
    gpio_set_direction(LED_GREEN_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_RED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_direction(BUILTIN_LED_GPIO, GPIO_MODE_OUTPUT);

    // Выключаем встроенный диод со старта
    gpio_set_level(BUILTIN_LED_GPIO, 0);

    // Настройка кнопки BOOT (при нажатии она замыкается на землю = выдает 0)
    gpio_set_direction(BOOT_BTN_GPIO, GPIO_MODE_INPUT);

    buzzer_init();

    // Инициализация АЦП
    adc_oneshot_unit_handle_t adc1_handle = NULL;
    adc_oneshot_unit_init_cfg_t init_config1 = { .unit_id = ADC_UNIT_1 };
    adc_oneshot_chan_cfg_t config = { .bitwidth = ADC_BITWIDTH_DEFAULT, .atten = ADC_ATTEN_DB_12 };
    bool adc_ready = adc_oneshot_new_unit(&init_config1, &adc1_handle) == ESP_OK &&
                     adc_oneshot_config_channel(adc1_handle, LIGHT_SENSOR_CHAN, &config) == ESP_OK;

    // Переменная для хранения предыдущего состояния кнопки
    int last_btn_state = 1;

    printf("Система запущена. Нажми кнопку BOOT для постановки на охрану!\n");

    while (1)
    {
        // --- 0. Чтение КНОПКИ BOOT ---
        int current_btn_state = gpio_get_level(BOOT_BTN_GPIO);

        // Ловим момент нажатия (было 1, стало 0)
        if (last_btn_state == 1 && current_btn_state == 0) {
            taskENTER_CRITICAL(&settings_lock);
            settings.armed = !settings.armed;
            bool armed = settings.armed;
            taskEXIT_CRITICAL(&settings_lock);
            printf("\n==== РЕЖИМ ОХРАНЫ: %s ====\n\n", armed ? "ВКЛЮЧЕН (Синий диод горит)" : "ОТКЛЮЧЕН");
        }
        last_btn_state = current_btn_state;

        // --- 1. Сонар ---
        float distance = 0.0f;
        bool distance_valid = read_distance(&distance);

        // --- 2. Свет ---
        int raw_light = 0;
        bool light_valid = adc_ready &&
                           adc_oneshot_read(adc1_handle, LIGHT_SENSOR_CHAN, &raw_light) == ESP_OK;

        // --- 3. Климат (последнее значение из climate_task) ---
        climate_data_t c;
        taskENTER_CRITICAL(&climate_lock);
        c = climate;
        taskEXIT_CRITICAL(&climate_lock);
        taskENTER_CRITICAL(&settings_lock);
        security_settings_t current = settings;
        taskEXIT_CRITICAL(&settings_lock);
        bool too_close = current.armed && distance_valid && distance > 0.1f &&
                         distance < current.dist_threshold;
        bool bright = current.armed && light_valid && raw_light > current.light_threshold;
        bool overheat = current.armed && c.temp_valid && c.temp_c > current.temp_threshold;

        // --- 4. ЛОГИКА (Зависит от режима) ---
        gpio_set_level(BUILTIN_LED_GPIO, current.armed);
        if (current.siren) {
            static int police_tick = 0;
            police_tick++;
            if (police_tick >= 6) police_tick = 0;

            if (police_tick < 3) {
                gpio_set_level(LED_RED_GPIO, 1);
                gpio_set_level(LED_GREEN_GPIO, 0);
                ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 10);
                ledc_set_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0, 1000);
                ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
            } else {
                gpio_set_level(LED_RED_GPIO, 0);
                gpio_set_level(LED_GREEN_GPIO, 1);
                ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 10);
                ledc_set_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0, 500);
                ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
            }
        } else if (current.armed) {
            // Восстанавливаем частоту зуммера на случай если была сирена
            ledc_set_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0, LEDC_FREQUENCY);

            // Охрана ВКЛ: Лампочки и зуммер реагируют на датчики
            gpio_set_level(LED_GREEN_GPIO, overheat ? 1 : 0);

            gpio_set_level(LED_RED_GPIO, too_close);
            ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, bright ? LEDC_DUTY_QUIET : 0);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
        } else {
            // Охрана ВЫКЛ: Принудительно тушим внешние диоды и зуммер
            gpio_set_level(LED_GREEN_GPIO, 0);
            gpio_set_level(LED_RED_GPIO, 0);
            ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
        }

        const security_telemetry_t telemetry = {
            .flags = (current.armed ? SECURITY_ARMED : 0) |
                     (current.siren ? SECURITY_SIREN : 0) |
                     (too_close ? SECURITY_TOO_CLOSE : 0) |
                     (bright ? SECURITY_BRIGHT : 0) |
                     (overheat ? SECURITY_OVERHEAT : 0),
            .distance_tenths_cm = distance_valid ? lroundf(distance * 10.0f) : UINT16_MAX,
            .light_adc = light_valid ? raw_light : UINT16_MAX,
            .temperature_tenths_c = c.temp_valid ? lroundf(c.temp_c * 10.0f) : INT16_MIN,
            .humidity_tenths_pct = c.hum_valid ? lroundf(c.hum_pct * 10.0f) : UINT16_MAX,
            .pressure_tenths_hpa = c.press_valid && c.press_hpa >= 0.0f && c.press_hpa < 6553.4f
                ? lroundf(c.press_hpa * 10.0f) : UINT16_MAX,
        };
        security_ble_publish(&telemetry);

        // Вывод статуса системы раз в 100мс.
        // Формат разбирает backend (SerialService): если нет данных — пишем "--".
        char temp_s[12] = "--", hum_s[12] = "--", press_s[12] = "--";
        if (c.temp_valid)  snprintf(temp_s, sizeof(temp_s), "%.1f", c.temp_c);
        if (c.hum_valid)   snprintf(hum_s, sizeof(hum_s), "%.1f", c.hum_pct);
        if (c.press_valid) snprintf(press_s, sizeof(press_s), "%.1f", c.press_hpa);

        printf("[%s] Дист: %4.1f см | Свет: %4d | Темп: %s C | Влаж: %s %% | Давл: %s гПа\n",
               current.armed ? "ОХРАНА" : " ЖДУЩ ", distance, raw_light, temp_s, hum_s, press_s);

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* ======================= Команды по UART ======================= */

static void uart_task(void *arg)
{
    // Настраиваем UART0
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    int intr_alloc_flags = 0;

    uart_driver_install(EX_UART_NUM, BUF_SIZE * 2, 0, 0, NULL, intr_alloc_flags);
    uart_param_config(EX_UART_NUM, &uart_config);
    uart_set_pin(EX_UART_NUM, UART_PIN_NO_CHANGE,
        UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    uint8_t *data = (uint8_t *) malloc(BUF_SIZE);

    while (1) {
        int len = uart_read_bytes(EX_UART_NUM, data, BUF_SIZE - 1, 20 / portTICK_PERIOD_MS);
        if (len > 0) {
            data[len] = '\0';
            // Парсим команды ARM / DISARM / DIST / LIGHT / TEMP / POLICE / NOPOLICE
            if (strncmp((char *)data, "DISARM", 6) == 0) {
                apply_security_command(SECURITY_DISARM);
                printf("\n==== РЕЖИМ ОХРАНЫ: ОТКЛЮЧЕН (ПО КОМАНДЕ ИЗ TELEGRAM) ====\n\n");
            } else if (strncmp((char *)data, "ARM", 3) == 0) {
                apply_security_command(SECURITY_ARM);
                printf("\n==== РЕЖИМ ОХРАНЫ: ВКЛЮЧЕН (ПО КОМАНДЕ ИЗ TELEGRAM) ====\n\n");
            } else if (strncmp((char *)data, "DIST:", 5) == 0) {
                float threshold = atof((char *)data + 5);
                taskENTER_CRITICAL(&settings_lock);
                settings.dist_threshold = threshold;
                taskEXIT_CRITICAL(&settings_lock);
                printf("\n==== НОВЫЙ ПОРОГ ДИСТАНЦИИ: %.1f ====\n\n", threshold);
            } else if (strncmp((char *)data, "LIGHT:", 6) == 0) {
                int threshold = atoi((char *)data + 6);
                taskENTER_CRITICAL(&settings_lock);
                settings.light_threshold = threshold;
                taskEXIT_CRITICAL(&settings_lock);
                printf("\n==== НОВЫЙ ПОРОГ СВЕТА: %d ====\n\n", threshold);
            } else if (strncmp((char *)data, "TEMP:", 5) == 0) {
                float threshold = atof((char *)data + 5);
                taskENTER_CRITICAL(&settings_lock);
                settings.temp_threshold = threshold;
                taskEXIT_CRITICAL(&settings_lock);
                printf("\n==== НОВЫЙ ПОРОГ ТЕМПЕРАТУРЫ: %.1f ====\n\n", threshold);
            } else if (strncmp((char *)data, "NOPOLICE", 8) == 0) {
                apply_security_command(SECURITY_SIREN_OFF);
                printf("\n==== РЕЖИМ СИРЕНЫ: ОТКЛЮЧЕН ====\n\n");
            } else if (strncmp((char *)data, "POLICE", 6) == 0) {
                apply_security_command(SECURITY_SIREN_ON);
                printf("\n==== РЕЖИМ СИРЕНЫ: ВКЛЮЧЕН ====\n\n");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void app_main(void)
{
    xTaskCreate(sensor_task, "sensor_task", 4096, NULL, 5, NULL);
    xTaskCreate(uart_task, "uart_task", 4096, NULL, 5, NULL);
    xTaskCreate(climate_task, "climate_task", 4096, NULL, 4, NULL);
    esp_err_t err = security_ble_start(apply_security_command);
    if (err != ESP_OK) {
        ESP_LOGE("security_ble", "BLE unavailable: %s", esp_err_to_name(err));
    }
}
