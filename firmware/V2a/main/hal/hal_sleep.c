#include "hal.h"
#include "hal_pins.h"
#include "esp_sleep.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "sleep";

void hal_sleep_init(void) { }

bool hal_sleep_was_timer_wake(void) {
    return (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER);
}

static void prepare_button_inputs(void) {
    gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL << PIN_BTN_A) | (1ULL << PIN_BTN_B),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn_cfg);
    gpio_hold_en(PIN_BTN_A);
    gpio_hold_en(PIN_BTN_B);
}

void hal_sleep_enter(uint32_t sleep_seconds) {
    ESP_LOGI(TAG, "sleeping %lus", (unsigned long)sleep_seconds);

    prepare_button_inputs();
    esp_sleep_enable_timer_wakeup((uint64_t)sleep_seconds * 1000000ULL);
    esp_deep_sleep_enable_gpio_wakeup(
        (1ULL << PIN_BTN_A) | (1ULL << PIN_BTN_B),
        ESP_GPIO_WAKEUP_GPIO_LOW
    );
    esp_deep_sleep_start();
}

void hal_sleep_enter_until_a(void) {
    ESP_LOGI(TAG, "sleeping until A press");

    prepare_button_inputs();
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
    esp_deep_sleep_enable_gpio_wakeup((1ULL << PIN_BTN_A), ESP_GPIO_WAKEUP_GPIO_LOW);
    esp_deep_sleep_start();
}

void hal_sleep_enter_indefinite(void) {
    ESP_LOGI(TAG, "indefinite deep sleep");
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
    esp_deep_sleep_start();
}
