#include "drivers/epd/epd.h"
#include "drivers/epd/epd_gfx.h"
#include "tests/epd_hello_test.h"
#ifdef EPD_HELLO_TEST
#include "esp_rom_sys.h"
#endif

#ifndef EPD_HELLO_TEST
#include <stdio.h>
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

// Framebuffers in regular SRAM (not RTC slow memory)
static uint8_t fb[EPD_FB_SIZE];
static uint8_t prev_fb[EPD_FB_SIZE];

// Wi-Fi / NTP policy
#define WIFI_CONNECT_TIMEOUT_MS    10000U
#define WIFI_RECONNECT_EVERY_MS    (60U * 60U * 1000U)
#define WIFI_SYNC_EVERY_MS         (10U * 60U * 1000U)
#define NTP_WAIT_TIMEOUT_MS        10000U

// Button policies
#define BUTTON_LONG_PRESS_MS       5000U
#define CLOCK_DOUBLE_TAP_MS        1000U

// Sleep policies
#define CLOCK_SLEEP_SECONDS        60U
#define CLOCK_SLEEP_GRACE_MS       1200U
#define ENERGY_AWAKE_MS            (2U * 60U * 1000U)
#define BATT_CHECK_MS              30000U

static EventGroupHandle_t s_wifi_event_group = NULL;
static bool s_wifi_connected = false;
static bool s_wifi_ready = false;
static bool s_wifi_started = false;

enum {
    WIFI_CONNECTED_BIT = BIT0,
    WIFI_FAIL_BIT = BIT1,
};

// refresh_speed index -> update interval in milliseconds
static const uint32_t REFRESH_MS[REFRESH_SPEED_COUNT] = {100, 250, 500, 1000, 10000, 60000};

// deghost_idx -> ms between full-white deghost refresh (0 = disabled)
static const uint32_t DEGHOST_MS[DEGHOST_IDX_COUNT] = {5 * 60 * 1000, 15 * 60 * 1000, 30 * 60 * 1000, 0};
// Avoid extra full-white cycles when only a few partial updates have happened.
#define DEGHOST_MIN_PARTIALS 8U

static void render_mode_frame(t1e_state_t *s) {
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

static void render_screensaver(void) {
    gfx_clear(fb, GFX_WHITE);
    gfx_puts_centered(fb, 88, "ZZZ", FONT_LARGE, GFX_BLACK);
    gfx_puts_centered(fb, 124, ".....", FONT_LARGE, GFX_BLACK);
    gfx_puts_centered(fb, 182, "PRESS A", FONT_SMALL, GFX_BLACK);
}

static void render(t1e_state_t *s) {
    if (s->mode == MODE_CLOCK && s->screensaver_active) {
        render_screensaver();
    } else {
        render_mode_frame(s);
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
    s_wifi_started = true;
    return true;
}

static void wifi_power_off(void) {
    if (!s_wifi_ready) return;
    if (s_wifi_started) {
        esp_wifi_disconnect();
        esp_wifi_stop();
    }
    s_wifi_connected = false;
    s_wifi_started = false;
}

static bool wifi_try_connect(uint32_t timeout_ms) {
    if (!s_wifi_ready || !s_wifi_event_group) return false;
    if (!s_wifi_started) {
        if (esp_wifi_start() != ESP_OK) return false;
        s_wifi_started = true;
    }

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
    if (!s_wifi_connected) {
        wifi_power_off();
        return false;
    }

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
        wifi_power_off();
        return false;
    }

    now += (time_t)T1E_WIFI_UTC_OFFSET_SECONDS;
    if (!rtc_set_epoch((uint32_t)now)) {
        ESP_LOGW(TAG, "RTC update failed");
        wifi_power_off();
        return false;
    }

    ESP_LOGI(TAG, "RTC updated from NTP");
    wifi_power_off();
    return true;
}

static uint8_t month_from_abbr(const char *abbr) {
    static const char *months[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    for (uint8_t i = 0; i < 12; i++) {
        if (strncmp(abbr, months[i], 3) == 0) return (uint8_t)(i + 1);
    }
    return 0;
}

static void rtc_restore_from_build_time_if_needed(void) {
    if (!rtc_lost_power()) return;

    char mon[4] = {0};
    int day = 0, year = 0;
    int hour = 0, minute = 0, sec = 0;
    if (sscanf(__DATE__, "%3s %d %d", mon, &day, &year) != 3) {
        ESP_LOGW(TAG, "RTC lost power: failed to parse build date");
        return;
    }
    if (sscanf(__TIME__, "%d:%d:%d", &hour, &minute, &sec) != 3) {
        ESP_LOGW(TAG, "RTC lost power: failed to parse build time");
        return;
    }

    uint8_t month = month_from_abbr(mon);
    if (month == 0 || day < 1 || day > 31 || year < 2000) {
        ESP_LOGW(TAG, "RTC lost power: invalid build timestamp");
        return;
    }

    rtc_time_t t = {
        .sec = (uint8_t)sec,
        .min = (uint8_t)minute,
        .hour = (uint8_t)hour,
        .dow = 1,
        .day = (uint8_t)day,
        .month = month,
        .year = (uint16_t)year,
    };
    if (rtc_set_time(&t)) {
        ESP_LOGW(TAG, "RTC lost power: set to build time %s %s", __DATE__, __TIME__);
    } else {
        ESP_LOGE(TAG, "RTC lost power: failed to write build time to RTC");
    }
}

static void handle_b_short_press(t1e_state_t *s, bool *need_refresh, bool *force_full) {
    if (s->mode == MODE_SETTINGS) {
        uint8_t invert_before = s->display_invert;
        mode_settings_confirm(s);
        if (s->display_invert != invert_before) *force_full = true;
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
    *need_refresh = true;
}
#endif

void app_main(void) {
#ifdef EPD_HELLO_TEST
    esp_rom_printf("T1E hello: app_main\n");
    epd_hello_test_run();
#else
    ESP_LOGI(TAG, "=== T1E BOOT ===");

    esp_sleep_wakeup_cause_t wake_cause = esp_sleep_get_wakeup_cause();
    uint64_t wake_mask = esp_sleep_get_gpio_wakeup_status();
    bool wake_by_a = (wake_mask & (1ULL << PIN_BTN_A)) != 0;

    hal_buttons_init();
    hal_power_init();
    if (hal_power_is_critical()) {
        ESP_LOGE(TAG, "Battery critically low, entering indefinite sleep");
        hal_sleep_enter_indefinite();
    }

    bool rtc_ok = rtc_init();
    if (!rtc_ok) {
        ESP_LOGE(TAG, "RTC FAILED");
    } else {
        rtc_restore_from_build_time_if_needed();
    }
    epd_init();

    t1e_state_t *s = state_get();
    if (s->magic != STATE_MAGIC) {
        state_init_defaults(s);
    }

    uint32_t now = xTaskGetTickCount();
    uint32_t energy_awake_deadline = 0;
    if (s->energy_saver_enabled && s->mode == MODE_CLOCK) {
        if (s->screensaver_active && wake_by_a && wake_cause == ESP_SLEEP_WAKEUP_GPIO) {
            s->screensaver_active = 0;
            energy_awake_deadline = now + pdMS_TO_TICKS(ENERGY_AWAKE_MS);
        } else if (!s->screensaver_active) {
            energy_awake_deadline = now + pdMS_TO_TICKS(ENERGY_AWAKE_MS);
        }
    }

    bool wifi_available = wifi_init_sta();
    if (wifi_available && !s->wifi_user_enabled) {
        wifi_power_off();
    }

    static int last_min = -1;

    render(s);
    if (s->display_invert) {
        for (int i = 0; i < EPD_FB_SIZE; i++) fb[i] ^= 0xFF;
    }
    epd_refresh_full(fb);
    memcpy(prev_fb, fb, EPD_FB_SIZE);

    rtc_time_t boot_time = {0};
    if (rtc_get_time(&boot_time)) {
        last_min = boot_time.min;
    }

    if (s->energy_saver_enabled && s->mode == MODE_CLOCK && s->screensaver_active) {
        hal_sleep_enter_until_a();
    }

    uint32_t last_refresh = xTaskGetTickCount();
    uint32_t ghost_timer  = xTaskGetTickCount();
    uint32_t next_wifi_sync = xTaskGetTickCount();
    uint32_t next_wifi_reconnect = xTaskGetTickCount();
    uint32_t next_batt_check = xTaskGetTickCount() + pdMS_TO_TICKS(BATT_CHECK_MS);

    bool a_down = false;
    bool b_down = false;
    bool a_long_handled = false;
    bool b_long_handled = false;
    uint32_t a_press_start = 0;
    uint32_t b_press_start = 0;
    bool a_tap_pending = false;
    uint32_t a_tap_deadline = 0;

    bool pending_clock_sleep = (s->mode == MODE_CLOCK && !s->energy_saver_enabled && !s->screensaver_active);
    uint32_t clock_sleep_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(CLOCK_SLEEP_GRACE_MS);

    while (1) {
        now = xTaskGetTickCount();
        btn_state_t btn = hal_buttons_read();
        bool need_refresh = false;
        bool force_full = false;
        bool do_deghost = false;

        if (tick_reached(now, next_batt_check)) {
            next_batt_check = now + pdMS_TO_TICKS(BATT_CHECK_MS);
            if (hal_power_is_critical()) {
                ESP_LOGE(TAG, "Battery critically low, entering indefinite sleep");
                hal_sleep_enter_indefinite();
            }
        }

        if (btn.raw_a && !a_down) {
            a_down = true;
            a_long_handled = false;
            a_press_start = now;
        }
        if (btn.raw_b && !b_down) {
            b_down = true;
            b_long_handled = false;
            b_press_start = now;
        }

        if (a_down && btn.raw_a && !a_long_handled && (now - a_press_start >= pdMS_TO_TICKS(BUTTON_LONG_PRESS_MS))) {
            a_long_handled = true;
            a_tap_pending = false;
            if (!wifi_available) {
                ESP_LOGW(TAG, "Wi-Fi toggle ignored: Wi-Fi credentials missing");
            } else {
                s->wifi_user_enabled = !s->wifi_user_enabled;
                ESP_LOGI(TAG, "Wi-Fi toggled %s", s->wifi_user_enabled ? "ON" : "OFF");
                if (!s->wifi_user_enabled) {
                    wifi_power_off();
                } else {
                    if (wifi_try_connect(WIFI_CONNECT_TIMEOUT_MS)) {
                        if (ntp_sync_rtc_once()) need_refresh = true;
                        next_wifi_sync = now + pdMS_TO_TICKS(WIFI_SYNC_EVERY_MS);
                    } else {
                        ESP_LOGW(TAG, "Wi-Fi connect failed after toggle ON");
                    }
                    next_wifi_reconnect = now + pdMS_TO_TICKS(WIFI_RECONNECT_EVERY_MS);
                }
            }
        }

        if (b_down && btn.raw_b && !b_long_handled && (now - b_press_start >= pdMS_TO_TICKS(BUTTON_LONG_PRESS_MS))) {
            b_long_handled = true;
            a_tap_pending = false;
            s->energy_saver_enabled = !s->energy_saver_enabled;
            ESP_LOGI(TAG, "Energy saver toggled %s", s->energy_saver_enabled ? "ON" : "OFF");
            if (s->energy_saver_enabled) {
                s->mode = MODE_CLOCK;
                s->screensaver_active = 1;
                energy_awake_deadline = 0;
            } else {
                s->screensaver_active = 0;
                energy_awake_deadline = now + pdMS_TO_TICKS(ENERGY_AWAKE_MS);
            }
            need_refresh = true;
        }

        if (!btn.raw_a && a_down) {
            bool was_long = a_long_handled;
            uint32_t held_ms = now - a_press_start;
            a_down = false;
            a_long_handled = false;

            if (!was_long && held_ms >= pdMS_TO_TICKS(30)) {
                if (s->mode == MODE_CLOCK) {
                    if (s->screensaver_active) {
                        s->screensaver_active = 0;
                        if (s->energy_saver_enabled) {
                            energy_awake_deadline = now + pdMS_TO_TICKS(ENERGY_AWAKE_MS);
                        }
                        need_refresh = true;
                    } else if (a_tap_pending && !tick_reached(now, a_tap_deadline)) {
                        a_tap_pending = false;
                        mode_clock_action(s);
                        need_refresh = true;
                    } else {
                        a_tap_pending = true;
                        a_tap_deadline = now + pdMS_TO_TICKS(CLOCK_DOUBLE_TAP_MS);
                    }
                } else {
                    action(s);
                    need_refresh = true;
                }
            }
        }

        if (!btn.raw_b && b_down) {
            bool was_long = b_long_handled;
            uint32_t held_ms = now - b_press_start;
            b_down = false;
            b_long_handled = false;

            if (!was_long && held_ms >= pdMS_TO_TICKS(30)) {
                if (!(s->mode == MODE_CLOCK && s->screensaver_active)) {
                    handle_b_short_press(s, &need_refresh, &force_full);
                }
            }
        }

        if (a_tap_pending && tick_reached(now, a_tap_deadline)) {
            a_tap_pending = false;
            if (s->mode == MODE_CLOCK && !s->screensaver_active) {
                s->screensaver_active = 1;
                need_refresh = true;
            }
        }

        if (s->energy_saver_enabled && s->mode == MODE_CLOCK && !s->screensaver_active) {
            if (energy_awake_deadline == 0) {
                energy_awake_deadline = now + pdMS_TO_TICKS(ENERGY_AWAKE_MS);
            } else if (tick_reached(now, energy_awake_deadline)) {
                s->screensaver_active = 1;
                energy_awake_deadline = 0;
                need_refresh = true;
            }
        }

        if (wifi_available && s->wifi_user_enabled) {
            if (!s_wifi_connected && tick_reached(now, next_wifi_reconnect)) {
                ESP_LOGI(TAG, "Wi-Fi connect attempt...");
                if (wifi_try_connect(WIFI_CONNECT_TIMEOUT_MS)) {
                    ESP_LOGI(TAG, "Wi-Fi connected");
                    next_wifi_sync = now;
                } else {
                    ESP_LOGW(TAG, "Wi-Fi connect failed");
                }
                next_wifi_reconnect = now + pdMS_TO_TICKS(WIFI_RECONNECT_EVERY_MS);
            }

            if (s_wifi_connected && tick_reached(now, next_wifi_sync)) {
                if (ntp_sync_rtc_once()) {
                    need_refresh = true;
                }
                next_wifi_sync = now + pdMS_TO_TICKS(WIFI_SYNC_EVERY_MS);
                next_wifi_reconnect = now + pdMS_TO_TICKS(WIFI_RECONNECT_EVERY_MS);
            }
        }

        if (!(s->mode == MODE_CLOCK && s->screensaver_active)) {
            uint32_t interval_ticks = pdMS_TO_TICKS(REFRESH_MS[s->refresh_speed % REFRESH_SPEED_COUNT]);
            if (s->mode == MODE_POMO && s->pomo_start) interval_ticks = pdMS_TO_TICKS(1000);

            if (s->mode == MODE_CLOCK || s->mode == MODE_LIFE_CLOCK) {
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
                if (now - last_refresh >= interval_ticks) {
                    need_refresh = true;
                    last_refresh = now;
                }
            }

            uint32_t dghost_ms = DEGHOST_MS[s->deghost_idx % DEGHOST_IDX_COUNT];
            if (dghost_ms > 0 &&
                now - ghost_timer >= pdMS_TO_TICKS(dghost_ms) &&
                s->partial_count >= DEGHOST_MIN_PARTIALS) {
                do_deghost = true;
                need_refresh = true;
                ghost_timer = now;
            }
        }

        if (need_refresh) {
            render(s);

            if (s->display_invert) {
                for (int i = 0; i < EPD_FB_SIZE; i++) fb[i] ^= 0xFF;
            }

            if (do_deghost) {
                epd_deghost(fb);
                s->partial_count = 0;
            } else if (force_full) {
                epd_refresh_full(fb);
                s->partial_count = 0;
            } else {
                epd_refresh_animate(prev_fb, fb, &s->partial_count);
            }

            memcpy(prev_fb, fb, EPD_FB_SIZE);

            if (s->energy_saver_enabled && s->mode == MODE_CLOCK && s->screensaver_active) {
                hal_sleep_enter_until_a();
            }

            pending_clock_sleep = (s->mode == MODE_CLOCK && !s->energy_saver_enabled && !s->screensaver_active);
            if (pending_clock_sleep) {
                clock_sleep_deadline = now + pdMS_TO_TICKS(CLOCK_SLEEP_GRACE_MS);
            }
        }

        if (pending_clock_sleep &&
            tick_reached(now, clock_sleep_deadline) &&
            !a_down && !b_down && !a_tap_pending) {
            hal_sleep_enter(CLOCK_SLEEP_SECONDS);
        }

        vTaskDelay(pdMS_TO_TICKS(30));
    }
#endif
}
