#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/ledc.h"        
#include "esp_timer.h"          
#include "esp_rom_sys.h"        
#include "esp_adc/adc_oneshot.h" 
#include "driver/uart.h"
#include <string.h>

#define EX_UART_NUM UART_NUM_0
#define BUF_SIZE (1024)

// --- Пины датчиков ---
#define TRIG_GPIO 5
#define ECHO_GPIO 18
#define LIGHT_SENSOR_CHAN ADC_CHANNEL_6 
#define PIR_GPIO 13                     

// --- Пины индикации и кнопок ---
#define LED_GREEN_GPIO 25       // Внешний Зеленый (Движение)
#define LED_RED_GPIO 26         // Внешний Красный (Дистанция < 5)
#define BOOT_BTN_GPIO 0         // Встроенная кнопка BOOT
#define BUILTIN_LED_GPIO 2      // Встроенный синий светодиод на плате

// --- Настройки зуммера ---
#define BUZZER_GPIO 17
#define LEDC_FREQUENCY 800              
#define LEDC_DUTY_QUIET 3                

// Глобальные переменные состояния
volatile bool is_armed = false;
volatile bool is_police = false;
volatile float dist_threshold = 5.0;
volatile int light_threshold = 800;

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

static void sensor_task(void *arg)
{
    // Инициализация датчиков
    gpio_set_direction(TRIG_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_direction(ECHO_GPIO, GPIO_MODE_INPUT);
    gpio_set_direction(PIR_GPIO, GPIO_MODE_INPUT); 
    gpio_pulldown_en(PIR_GPIO); 

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
    adc_oneshot_unit_handle_t adc1_handle;
    adc_oneshot_unit_init_cfg_t init_config1 = { .unit_id = ADC_UNIT_1 };
    adc_oneshot_new_unit(&init_config1, &adc1_handle);
    adc_oneshot_chan_cfg_t config = { .bitwidth = ADC_BITWIDTH_DEFAULT, .atten = ADC_ATTEN_DB_12 };
    adc_oneshot_config_channel(adc1_handle, LIGHT_SENSOR_CHAN, &config);

    // Переменная для хранения предыдущего состояния кнопки
    int last_btn_state = 1; 

    printf("Система запущена. Нажми кнопку BOOT для постановки на охрану!\n");

    while (1)
    {
        // --- 0. Чтение КНОПКИ BOOT ---
        int current_btn_state = gpio_get_level(BOOT_BTN_GPIO);
        
        // Ловим момент нажатия (было 1, стало 0)
        if (last_btn_state == 1 && current_btn_state == 0) {
            is_armed = !is_armed; // Переключаем режим!
            
            // Зажигаем или тушим синий светодиод на плате
            gpio_set_level(BUILTIN_LED_GPIO, is_armed ? 1 : 0);
            
            printf("\n==== РЕЖИМ ОХРАНЫ: %s ====\n\n", is_armed ? "ВКЛЮЧЕН (Синий диод горит)" : "ОТКЛЮЧЕН");
        }
        last_btn_state = current_btn_state;

        // --- 1. Сонар ---
        gpio_set_level(TRIG_GPIO, 0);
        esp_rom_delay_us(2);
        gpio_set_level(TRIG_GPIO, 1);
        esp_rom_delay_us(10);
        gpio_set_level(TRIG_GPIO, 0);

        int64_t start_time = esp_timer_get_time();
        while (gpio_get_level(ECHO_GPIO) == 0 && (esp_timer_get_time() - start_time < 30000));
        int64_t echo_start = esp_timer_get_time();
        while (gpio_get_level(ECHO_GPIO) == 1 && (esp_timer_get_time() - echo_start < 30000));
        int64_t echo_end = esp_timer_get_time();
        float distance = (echo_end - echo_start) * 0.0343 / 2.0;

        // --- 2. Свет ---
        int raw_light = 0;
        adc_oneshot_read(adc1_handle, LIGHT_SENSOR_CHAN, &raw_light);

        // --- 3. Движение ---
        static int motion_debounce_counter = 0;
        int raw_motion = gpio_get_level(PIR_GPIO);
        if (raw_motion == 1) {
            motion_debounce_counter++;
        } else {
            motion_debounce_counter = 0;
        }
        // Считаем уверенным движением, если сигнал держится минимум 3 тика (300 мс)
        int motion_detected = (motion_debounce_counter >= 3) ? 1 : 0;
        
        // --- 4. ЛОГИКА (Зависит от режима) ---
        if (is_police) {
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
        } else if (is_armed) {
            // Восстанавливаем частоту зуммера на случай если была сирена
            ledc_set_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0, LEDC_FREQUENCY);
            
            // Охрана ВКЛ: Лампочки и зуммер реагируют на датчики
            gpio_set_level(LED_GREEN_GPIO, motion_detected);
            
            if (distance < dist_threshold && distance > 0.1) {
                gpio_set_level(LED_RED_GPIO, 1);
            } else {
                gpio_set_level(LED_RED_GPIO, 0);
            }

            if (raw_light > light_threshold) {
                ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, LEDC_DUTY_QUIET);
                ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
            } else {
                ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
                ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
            }
        } else {
            // Охрана ВЫКЛ: Принудительно тушим внешние диоды и зуммер
            gpio_set_level(LED_GREEN_GPIO, 0);
            gpio_set_level(LED_RED_GPIO, 0);
            ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
        }

        // Вывод статуса системы раз в 100мс
        printf("[%s] Дист: %4.1f см | Свет: %4d | Движ: %d\n", 
               is_armed ? "ОХРАНА" : " ЖДУЩ ", distance, raw_light, motion_detected);

        vTaskDelay(pdMS_TO_TICKS(100)); 
    }
}

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
    uart_set_pin(EX_UART_NUM, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    uint8_t *data = (uint8_t *) malloc(BUF_SIZE);
    
    while (1) {
        int len = uart_read_bytes(EX_UART_NUM, data, BUF_SIZE - 1, 20 / portTICK_PERIOD_MS);
        if (len > 0) {
            data[len] = '\0';
            // Парсим команды ARM / DISARM / DIST / LIGHT
            if (strncmp((char *)data, "DISARM", 6) == 0) {
                is_armed = false;
                gpio_set_level(BUILTIN_LED_GPIO, 0);
                printf("\n==== РЕЖИМ ОХРАНЫ: ОТКЛЮЧЕН (ПО КОМАНДЕ ИЗ TELEGRAM) ====\n\n");
            } else if (strncmp((char *)data, "ARM", 3) == 0) {
                is_armed = true;
                gpio_set_level(BUILTIN_LED_GPIO, 1);
                printf("\n==== РЕЖИМ ОХРАНЫ: ВКЛЮЧЕН (ПО КОМАНДЕ ИЗ TELEGRAM) ====\n\n");
            } else if (strncmp((char *)data, "DIST:", 5) == 0) {
                dist_threshold = atof((char *)data + 5);
                printf("\n==== НОВЫЙ ПОРОГ ДИСТАНЦИИ: %.1f ====\n\n", dist_threshold);
            } else if (strncmp((char *)data, "LIGHT:", 6) == 0) {
                light_threshold = atoi((char *)data + 6);
                printf("\n==== НОВЫЙ ПОРОГ СВЕТА: %d ====\n\n", light_threshold);
            } else if (strncmp((char *)data, "NOPOLICE", 8) == 0) {
                is_police = false;
                printf("\n==== РЕЖИМ СИРЕНЫ: ОТКЛЮЧЕН ====\n\n");
            } else if (strncmp((char *)data, "POLICE", 6) == 0) {
                is_police = true;
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
}