#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/uart.h"
#include "climate.h"
#include "cloud.h"
#include "esp_timer.h"
#include "sonar.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "security_ble.h"

#define EX_UART_NUM UART_NUM_0
#define BUF_SIZE (1024)

// --- Пины датчиков ---
#define TRIG_GPIO 5
#define ECHO_GPIO 18
#define LIGHT_SENSOR_CHAN ADC_CHANNEL_6  // GPIO34

// --- Пины индикации и кнопок ---
#define LED_GREEN_GPIO 25       // Внешний Зеленый (Перегрев: температура > порога)
#define LED_RED_GPIO 26         // Внешний Красный (Дистанция < порога)
#define BOOT_BTN_GPIO 0         // Встроенная кнопка BOOT
#define BUILTIN_LED_GPIO 2      // Встроенный синий светодиод на плате

// --- Настройки зуммера ---
#define BUZZER_GPIO 17
#define LEDC_FREQUENCY 800
#define LEDC_DUTY_QUIET 3

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

static climate_data_t climate = {0};
static portMUX_TYPE climate_lock = portMUX_INITIALIZER_UNLOCKED;

static void update_climate(const climate_data_t *sample)
{
    taskENTER_CRITICAL(&climate_lock);
    climate = *sample;
    taskEXIT_CRITICAL(&climate_lock);
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

static void sensor_task(void *arg)
{
    // Инициализация датчиков
    esp_err_t sonar_err = sonar_init(TRIG_GPIO, ECHO_GPIO);
    if (sonar_err != ESP_OK) {
        ESP_LOGE("sonar", "RMT init failed: %s", esp_err_to_name(sonar_err));
    }

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
        bool distance_valid = sonar_read(&distance);

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

        // Вывод статуса после каждого цикла измерения.
        // Диагностика в Serial Monitor: если нет данных — пишем "--".
        char dist_s[12] = "--", temp_s[12] = "--", hum_s[12] = "--", press_s[12] = "--";
        if (distance_valid) snprintf(dist_s, sizeof(dist_s), "%.1f", distance);
        if (c.temp_valid)  snprintf(temp_s, sizeof(temp_s), "%.1f", c.temp_c);
        if (c.hum_valid)   snprintf(hum_s, sizeof(hum_s), "%.1f", c.hum_pct);
        if (c.press_valid) snprintf(press_s, sizeof(press_s), "%.1f", c.press_hpa);

        printf("[%s] Дист: %s см | Свет: %4d | Темп: %s C | Влаж: %s %% | Давл: %s гПа\n",
               current.armed ? "ОХРАНА" : " ЖДУЩ ", dist_s, raw_light, temp_s, hum_s, press_s);

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
    esp_err_t cloud_err = cloud_start(apply_security_command);
    if (cloud_err != ESP_OK) {
        ESP_LOGE("cloud", "Cloud disabled, local control remains available: %s", esp_err_to_name(cloud_err));
    }
    xTaskCreate(sensor_task, "sensor_task", 4096, NULL, 5, NULL);
    xTaskCreate(uart_task, "uart_task", 4096, NULL, 5, NULL);
    ESP_ERROR_CHECK(climate_start(update_climate));
    esp_err_t err = security_ble_start(apply_security_command);
    if (err != ESP_OK) {
        ESP_LOGE("security_ble", "BLE unavailable: %s", esp_err_to_name(err));
    }
}
