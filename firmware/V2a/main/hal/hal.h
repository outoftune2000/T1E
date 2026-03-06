#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool a;
    bool b;
    bool both;
    bool raw_a;
    bool raw_b;
} btn_state_t;

void hal_buttons_init(void);
btn_state_t hal_buttons_read(void);

void hal_sleep_init(void);
void hal_sleep_enter(uint32_t sleep_seconds);
void hal_sleep_enter_until_a(void);
void hal_sleep_enter_indefinite(void);
bool hal_sleep_was_timer_wake(void);

void hal_power_init(void);
float hal_power_battery_voltage(void);
int hal_power_battery_percent(void);
bool hal_power_is_critical(void);
