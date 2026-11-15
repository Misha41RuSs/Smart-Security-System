#include "sonar.h"
#include "sonar_decode.h"
#include "driver/gpio.h"
#include "driver/rmt_rx.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_log.h"

static rmt_channel_handle_t rx, tx;
static rmt_encoder_handle_t encoder;
static QueueHandle_t completed;
static rmt_symbol_word_t symbols[64];
static int echo_pin;
static bool ready;
static const rmt_symbol_word_t trigger = {
    .level0 = 0, .duration0 = 2, .level1 = 1, .duration1 = 10,
};

static bool receive_done(rmt_channel_handle_t channel, const rmt_rx_done_event_data_t *event, void *context)
{
    BaseType_t wake = pdFALSE;
    xQueueSendFromISR(completed, &event->num_symbols, &wake);
    return wake == pdTRUE;
}

esp_err_t sonar_init(int trigger_gpio, int echo_gpio)
{
    echo_pin = echo_gpio;
    completed = xQueueCreate(1, sizeof(size_t));
    if (!completed) return ESP_ERR_NO_MEM;
    rmt_rx_channel_config_t rx_config = {
        .gpio_num = echo_gpio, .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 1000000, .mem_block_symbols = 64,
    };
    rmt_tx_channel_config_t tx_config = {
        .gpio_num = trigger_gpio, .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 1000000, .mem_block_symbols = 64, .trans_queue_depth = 1,
    };
    rmt_copy_encoder_config_t copy_config = {0};
    rmt_rx_event_callbacks_t callbacks = {.on_recv_done = receive_done};
    bool rx_enabled = false;
    esp_err_t err = rmt_new_rx_channel(&rx_config, &rx);
    if (err == ESP_OK) err = rmt_new_tx_channel(&tx_config, &tx);
    if (err == ESP_OK) err = rmt_new_copy_encoder(&copy_config, &encoder);
    if (err == ESP_OK) err = rmt_rx_register_event_callbacks(rx, &callbacks, NULL);
    if (err == ESP_OK) {
        err = rmt_enable(rx);
        rx_enabled = err == ESP_OK;
    }
    if (err == ESP_OK) err = rmt_enable(tx);
    if (err != ESP_OK) {
        if (rx_enabled) rmt_disable(rx);
        if (encoder) rmt_del_encoder(encoder);
        if (tx) rmt_del_channel(tx);
        if (rx) rmt_del_channel(rx);
        vQueueDelete(completed);
        return err;
    }
    ready = true;
    ESP_LOGI("sonar", "RMT ready: TRIG=%d ECHO=%d, resolution 1 us", trigger_gpio, echo_gpio);
    return ESP_OK;
}

bool sonar_read(float *distance_cm)
{
    if (!ready || !distance_cm || gpio_get_level(echo_pin)) return false;
    xQueueReset(completed);
    const rmt_receive_config_t receive_config = {
        .signal_range_min_ns = 1000, .signal_range_max_ns = 30000000,
    };
    const rmt_transmit_config_t transmit_config = {.flags.eot_level = 0};
    if (rmt_receive(rx, symbols, sizeof(symbols), &receive_config) != ESP_OK) return false;
    esp_err_t err = rmt_transmit(tx, encoder, &trigger, sizeof(trigger), &transmit_config);
    if (err == ESP_OK) err = rmt_tx_wait_all_done(tx, 100);
    size_t count = 0;
    bool received = err == ESP_OK && xQueueReceive(completed, &count, pdMS_TO_TICKS(100)) == pdTRUE;
    if (!received) {
        // Abort before reusing the RX buffer, and discard any completion from
        // this failed transaction. No ISR timestamps or busy-wait echo loops.
        esp_err_t reset = rmt_disable(rx);
        if (reset == ESP_OK) reset = rmt_enable(rx);
        if (reset != ESP_OK) ready = false;
        return false;
    }
    if (count >= 64 || gpio_get_level(echo_pin)) return false;
    uint32_t words[64], pulse_us;
    for (size_t i = 0; i < count; i++) words[i] = symbols[i].val;
    if (!sonar_pulse_us(words, count, &pulse_us)) return false;
    *distance_cm = pulse_us * 0.0343f / 2.0f;
    return true;
}
