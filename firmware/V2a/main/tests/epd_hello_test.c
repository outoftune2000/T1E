#include "epd_hello_test.h"

#include <stdint.h>

#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "drivers/epd/epd.h"
#include "drivers/epd/epd_gfx.h"

void epd_hello_test_run(void) {
    static uint8_t fb[EPD_FB_SIZE];

    esp_rom_printf("T1E hello: test start\n");
    epd_init();
    esp_rom_printf("T1E hello: epd_init done\n");

    gfx_clear(fb, GFX_WHITE);
    gfx_puts_centered(fb, 112, "HELLO WORLD", FONT_MEDIUM, GFX_BLACK);
    gfx_puts_centered(fb, 140, "T1E EPD TEST", FONT_SMALL, GFX_BLACK);
    epd_refresh_full(fb);
    esp_rom_printf("T1E hello: refresh done\n");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
