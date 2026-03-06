#include "drivers/epd/epd.h"
#include "drivers/epd/epd_gfx.h"
#include "tests/epd_hello_test.h"
#ifdef EPD_HELLO_TEST
#include "esp_rom_sys.h"
#endif

#ifndef EPD_HELLO_TEST
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_attr.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "lwip/apps/sntp.h"

#include "hal/hal.h"
#include "hal/hal_pins.h"
#include "drivers/rtc/rtc_ds3231.h"
#include "wifi_credentials.h"
#include "app/state.h"
#include "app/modes/mode_clock.h"
#include "app/modes/mode_life_clock.h"
#include "app/modes/mode_dice.h"
#include "app/modes/mode_art.h"
#include "app/modes/mode_pomo.h"
#include "app/modes/mode_life.h"
#include "app/modes/mode_settings.h"

static const char *TAG = "t1e";

// Framebuffers in regular SRAM — NOT RTC slow memory
static uint8_t fb[EPD_FB_SIZE];
static uint8_t prev_fb[EPD_FB_SIZE];

// Wi-Fi / NTP policy
#define WIFI_CONNECT_TIMEOUT_MS    10000U
#define WIFI_RECONNECT_EVERY_MS    (60U * 60U * 1000U)
#define WIFI_SYNC_EVERY_MS         (10U * 60U * 1000U)
#define NTP_WAIT_TIMEOUT_MS        10000U

static EventGroupHandle_t s_wifi_event_group = NULL;
static bool s_wifi_connected = false;
static bool s_wifi_ready = false;

enum {
    WIFI_CONNECTED_BIT = BIT0,
    WIFI_FAIL_BIT = BIT1,
};

// refresh_speed index → update interval in milliseconds
static const uint32_t REFRESH_MS[REFRESH_SPEED_COUNT] = {100, 250, 500, 1000, 10000, 60000};

// deghost_idx → ms between full-white deghost refresh (0 = disabled)
static const uint32_t DEGHOST_MS[DEGHOST_IDX_COUNT] = {5*60*1000, 15*60*1000, 30*60*1000, 0};

static void render(t1e_state_t *s) {
    switch (s->mode) {
        case MODE_CLOCK:      mode_clock_render(fb, s);      break;
        case MODE_LIFE_CLOCK: mode_life_clock_render(fb, s); break;
        case MODE_DICE:       mode_dice_render(fb, s);       break;
        case MODE_ART:        mode_art_render(fb, s);        break;
        case MODE_POMO:       mode_pomo_render(fb, s);       break;
        case MODE_LIFE:       mode_life_render(fb, s);       break;
        case MODE_SETTINGS:   mode_settings_render(fb, s);   break;
        default:              mode_clock_render(fb, s);       break;
    }
}

static void action(t1e_state_t *s) {
    switch (s->mode) {
        case MODE_CLOCK:      mode_clock_action(s);      break;
        case MODE_LIFE_CLOCK: mode_life_clock_action(s); break;
        case MODE_DICE:       mode_dice_action(s);       break;
        case MODE_ART:        mode_art_action(s);        break;
        case MODE_POMO:       mode_pomo_action(s);       break;
        case MODE_LIFE:       mode_life_action(s);       break;
        case MODE_SETTINGS:   mode_settings_action(s);   break;
        default: break;
    }
}

static bool tick_reached(uint32_t now, uint32_t deadline) {
    return (int32_t)(now - deadline) >= 0;
}

static bool wifi_feature_enabled(void) {
    return (T1E_WIFI_SSID[0] != '\0');
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    (void)arg;
    (void)event_data;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_connected = false;
        if (s_wifi_event_group) xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_wifi_connected = true;
        if (s_wifi_event_group) xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static bool wifi_init_sta(void) {
    if (!wifi_feature_enabled()) {
        ESP_LOGI(TAG, "Wi-Fi disabled: set T1E_WIFI_SSID in wifi_credentials.h");
        return false;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(err));
        return false;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    s_wifi_event_group = xEventGroupCreate();
    if (!s_wifi_event_group) {
        ESP_LOGE(TAG, "Wi-Fi event group alloc failed");
        return false;
    }

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, T1E_WIFI_SSID, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, T1E_WIFI_PASSWORD, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    s_wifi_ready = true;
    return true;
}

static bool wifi_try_connect(uint32_t timeout_ms) {
    if (!s_wifi_ready || !s_wifi_event_group) return false;

    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    if (esp_wifi_connect() != ESP_OK) return false;

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdTRUE,
        pdFALSE,
        pdMS_TO_TICKS(timeout_ms));

    return (bits & WIFI_CONNECTED_BIT) != 0;
}

static bool ntp_sync_rtc_once(void) {
    if (!s_wifi_connected) return false;

    sntp_stop();
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, T1E_WIFI_NTP_SERVER);
    sntp_init();

    time_t now = 0;
    const uint32_t retry_sleep_ms = 250;
    const uint32_t retries = NTP_WAIT_TIMEOUT_MS / retry_sleep_ms;

    for (uint32_t i = 0; i < retries; i++) {
        time(&now);
        if (now > 1700000000) break;
        vTaskDelay(pdMS_TO_TICKS(retry_sleep_ms));
    }
    sntp_stop();

    if (now <= 1700000000) {
        ESP_LOGW(TAG, "NTP sync timeout");
        return false;
    }

    now += (time_t)T1E_WIFI_UTC_OFFSET_SECONDS;
    if (!rtc_set_epoch((uint32_t)now)) {
        ESP_LOGW(TAG, "RTC update failed");
        return false;
    }

    ESP_LOGI(TAG, "RTC updated from NTP");
    return true;
}
#endif

void app_main(void) {
#ifdef EPD_HELLO_TEST
    esp_rom_printf("T1E hello: app_main\n");
    epd_hello_test_run();
#else
    ESP_LOGI(TAG, "=== T1E BOOT ===");

    hal_buttons_init();
    hal_power_init();
    if (!rtc_init()) ESP_LOGE(TAG, "RTC FAILED");
    epd_init();

    t1e_state_t *s = state_get();
    if (s->magic != STATE_MAGIC) {
        state_init_defaults(s);
    }

    // Cold boot: full refresh to establish clean baseline in both RAMs
    render(s);
    if (s->display_invert)
        for (int i = 0; i < EPD_FB_SIZE; i++) fb[i] ^= 0xFF;
    epd_refresh_full(fb);
    memcpy(prev_fb, fb, EPD_FB_SIZE);

    uint32_t last_refresh = xTaskGetTickCount();
    uint32_t ghost_timer  = xTaskGetTickCount();
    bool wifi_enabled = wifi_init_sta();
    uint32_t next_wifi_sync = xTaskGetTickCount();
    uint32_t next_wifi_reconnect = xTaskGetTickCount();

    while (1) {
        btn_state_t btn = hal_buttons_read();
        bool need_refresh = false;
        bool force_full   = false;
        bool do_deghost   = false;

        // --- Button A: secondary action ---
        if (btn.a) {
            action(s);
            need_refresh = true;
        }

        // --- Button B: confirm / mode advance ---
        if (btn.b) {
            if (s->mode == MODE_SETTINGS) {
                uint8_t invert_before = s->display_invert;
                mode_settings_confirm(s);
                // If invert just toggled, force a full refresh to reset RAM baseline
                if (s->display_invert != invert_before) force_full = true;
            } else if (s->mode == MODE_DICE) {
                if (s->dice_type < DICE_COUNT - 1) {
                    s->dice_type++;
                    s->last_roll = 0;
                } else {
                    s->dice_type = DICE_D20;
                    s->last_roll = 0;
                    state_next_mode(s);
                }
            } else {
                state_next_mode(s);
            }
            need_refresh = true;
        }

        // --- Time-based refresh ---
        uint32_t now = xTaskGetTickCount(); // ticks (1 tick = 1ms at default ESP32 config)

        // --- Wi-Fi / NTP time sync ---
        if (wifi_enabled) {
            if (!s_wifi_connected && tick_reached(now, next_wifi_reconnect)) {
                ESP_LOGI(TAG, "Wi-Fi connect attempt...");
                if (wifi_try_connect(WIFI_CONNECT_TIMEOUT_MS)) {
                    ESP_LOGI(TAG, "Wi-Fi connected");
                    next_wifi_sync = now; // sync immediately on successful connect
                } else {
                    ESP_LOGW(TAG, "Wi-Fi connect failed");
                }
                next_wifi_reconnect = now + pdMS_TO_TICKS(WIFI_RECONNECT_EVERY_MS);
            }

            if (s_wifi_connected && tick_reached(now, next_wifi_sync)) {
                if (ntp_sync_rtc_once()) {
                    need_refresh = true; // clock/date may have changed
                }
                next_wifi_sync = now + pdMS_TO_TICKS(WIFI_SYNC_EVERY_MS);
            }
        }

        uint32_t interval_ticks = pdMS_TO_TICKS(REFRESH_MS[s->refresh_speed % REFRESH_SPEED_COUNT]);
        // POMO countdown always needs 1s resolution
        if (s->mode == MODE_POMO && s->pomo_start) interval_ticks = pdMS_TO_TICKS(1000);

        static int last_min = -1;
        if (s->mode == MODE_CLOCK || s->mode == MODE_LIFE_CLOCK) {
            // For clock modes, only refresh when the minute actually changes.
            // We ignore interval_ticks entirely, but poll gently every 1 second.
            if (now - last_refresh >= pdMS_TO_TICKS(1000)) {
                rtc_time_t t = {0};
                if (rtc_get_time(&t)) {
                    if (t.min != last_min) {
                        need_refresh = true;
                        last_min = t.min;
                    }
                }
                last_refresh = now;
            }
        } else {
            // Normal global refresh interval for non-clock modes
            if (now - last_refresh >= interval_ticks) {
                need_refresh = true;
                last_refresh = now;
            }
        }

        // --- Deghost watchdog ---
        // Banks display to all-white then partial-refreshes back to current frame.
        // This clears image retention without a jarring double-flash.
        uint32_t dghost_ms = DEGHOST_MS[s->deghost_idx % DEGHOST_IDX_COUNT];
        if (dghost_ms > 0 && now - ghost_timer >= pdMS_TO_TICKS(dghost_ms)) {
            do_deghost   = true;
            need_refresh = true;
            ghost_timer  = now;
        }

        // --- Render and refresh ---
        if (need_refresh) {
            render(s);

            // Apply inversion if enabled
            if (s->display_invert)
                for (int i = 0; i < EPD_FB_SIZE; i++) fb[i] ^= 0xFF;

            if (do_deghost) {
                // Full white refresh to clear ghosting, then partial back to frame
                epd_deghost(fb);
                s->partial_count = 0;
            } else if (force_full) {
                epd_refresh_full(fb);
                s->partial_count = 0;
            } else {
                epd_refresh_animate(prev_fb, fb, &s->partial_count);
            }

            memcpy(prev_fb, fb, EPD_FB_SIZE);
        }

        vTaskDelay(pdMS_TO_TICKS(30));
    }
#endif
}
