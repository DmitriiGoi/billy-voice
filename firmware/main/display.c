#include "display.h"
#include "config.h"
#include "esp_log.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "display";

// Waveshare ESP32-S3 1.85" Round LCD (GC9A01 контроллер)
#define LCD_HOST     SPI2_HOST
#define PIN_SCLK     GPIO_NUM_6
#define PIN_MOSI     GPIO_NUM_7
#define PIN_CS       GPIO_NUM_5
#define PIN_DC       GPIO_NUM_4
#define PIN_RST      GPIO_NUM_8
#define PIN_BL       GPIO_NUM_9

static esp_lcd_panel_handle_t panel = NULL;

// Простые цветные экраны под каждый стейт (16-bit RGB565)
#define COLOR_IDLE       0x0000   // чёрный — спим
#define COLOR_LISTENING  0x07E0   // зелёный — слушаем
#define COLOR_PROCESSING 0xFFE0   // жёлтый — думаем
#define COLOR_SPEAKING   0x001F   // синий — говорим
#define COLOR_ERROR      0xF800   // красный — ошибка

static uint16_t *fb = NULL;

void display_init(void)
{
    // SPI bus
    spi_bus_config_t bus_cfg = {
        .mosi_io_num   = PIN_MOSI,
        .miso_io_num   = -1,
        .sclk_io_num   = PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_WIDTH * LCD_HEIGHT * 2,
    };
    spi_bus_initialize(LCD_HOST, &bus_cfg, SPI_DMA_CH_AUTO);

    // LCD panel IO
    esp_lcd_panel_io_handle_t io_handle;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num       = PIN_DC,
        .cs_gpio_num       = PIN_CS,
        .pclk_hz           = 40 * 1000 * 1000,
        .lcd_cmd_bits      = 8,
        .lcd_param_bits    = 8,
        .spi_mode          = 0,
        .trans_queue_depth = 10,
    };
    esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &io_handle);

    // GC9A01 panel
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_RST,
        .rgb_endian     = LCD_RGB_ENDIAN_BGR,
        .bits_per_pixel = 16,
    };
    esp_lcd_new_panel_gc9a01(io_handle, &panel_cfg, &panel);

    esp_lcd_panel_reset(panel);
    esp_lcd_panel_init(panel);
    esp_lcd_panel_disp_on_off(panel, true);

    // Подсветка
    gpio_set_direction(PIN_BL, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_BL, 1);

    // Framebuffer — заполним чёрным
    fb = calloc(LCD_WIDTH * LCD_HEIGHT, sizeof(uint16_t));

    ESP_LOGI(TAG, "display init OK (%dx%d)", LCD_WIDTH, LCD_HEIGHT);
}

static void fill_color(uint16_t color)
{
    if (!fb) return;
    for (int i = 0; i < LCD_WIDTH * LCD_HEIGHT; i++) {
        fb[i] = color;
    }
    esp_lcd_panel_draw_bitmap(panel, 0, 0, LCD_WIDTH, LCD_HEIGHT, fb);
}

void display_set_state(billy_state_t state)
{
    switch (state) {
        case STATE_IDLE:       fill_color(COLOR_IDLE);       break;
        case STATE_LISTENING:  fill_color(COLOR_LISTENING);  break;
        case STATE_PROCESSING: fill_color(COLOR_PROCESSING); break;
        case STATE_SPEAKING:   fill_color(COLOR_SPEAKING);   break;
        case STATE_ERROR:      fill_color(COLOR_ERROR);      break;
    }
    ESP_LOGI(TAG, "display state -> %d", state);
}
