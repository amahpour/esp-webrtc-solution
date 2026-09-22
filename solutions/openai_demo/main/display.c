/* Status display on the ESP32-S3-BOX-3 LCD (320x240 over SPI3)

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.

   Pins and init sequence follow esp-bsp/bsp/esp-box-3. The BSP itself is not used because its
   display init also claims both I2C buses (codec + dock), which this demo already owns.
   BOX-3 ships with either an ILI9341 (GT911 touch) or an ST7789 (TT21100 touch) panel; like
   the BSP, we pick the driver by probing the touch controller on the codec I2C bus.
*/

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "display.h"
#include "codec_init.h"
#include "esp_check.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_lcd_ili9341.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#define TAG "DISPLAY"

#define LCD_H_RES        (320)
#define LCD_V_RES        (240)
#define LCD_SPI_HOST     (SPI3_HOST)
#define LCD_PIXEL_CLK_HZ (40 * 1000 * 1000)
#define LCD_GPIO_MOSI    (6)
#define LCD_GPIO_PCLK    (7)
#define LCD_GPIO_CS      (5)
#define LCD_GPIO_DC      (4)
#define LCD_GPIO_RST     (48)
#define LCD_GPIO_BL      (47)
#define LCD_DRAW_BUF_LINES (24) /* internal DMA RAM is scarce in this demo: 24 lines = 15 KB */
#define TOUCH_TT21100_ADDR (0x24) /* present only on the ST7789 panel variant */
#define BL_LEDC_CH       (LEDC_CHANNEL_1)
#define BL_LEDC_TIMER    (LEDC_TIMER_1)

#if LV_FONT_MONTSERRAT_24
#define FONT_TITLE (&lv_font_montserrat_24)
#else
#define FONT_TITLE LV_FONT_DEFAULT
#endif
#if LV_FONT_MONTSERRAT_20
#define FONT_BODY (&lv_font_montserrat_20)
#else
#define FONT_BODY LV_FONT_DEFAULT
#endif

static bool      initialized;
static bool      error_sticky;
static lv_obj_t *status_label;
static lv_obj_t *transcript_label;
static lv_obj_t *tool_label;

/* BOX-3 ILI9341 vendor init, verbatim from esp-bsp */
static const ili9341_lcd_init_cmd_t ili9341_box3_init[] = {
    {0xC8, (uint8_t []){0xFF, 0x93, 0x42}, 3, 0},
    {0xC0, (uint8_t []){0x0E, 0x0E}, 2, 0},
    {0xC5, (uint8_t []){0xD0}, 1, 0},
    {0xC1, (uint8_t []){0x02}, 1, 0},
    {0xB4, (uint8_t []){0x02}, 1, 0},
    {0xE0, (uint8_t []){0x00, 0x03, 0x08, 0x06, 0x13, 0x09, 0x39, 0x39, 0x48, 0x02, 0x0a, 0x08, 0x17, 0x17, 0x0F}, 15, 0},
    {0xE1, (uint8_t []){0x00, 0x28, 0x29, 0x01, 0x0d, 0x03, 0x3f, 0x33, 0x52, 0x04, 0x0f, 0x0e, 0x37, 0x38, 0x0F}, 15, 0},
    {0xB1, (uint8_t []){00, 0x1B}, 2, 0},
    {0x36, (uint8_t []){0x08}, 1, 0},
    {0x3A, (uint8_t []){0x55}, 1, 0},
    {0xB7, (uint8_t []){0x06}, 1, 0},
    {0x11, (uint8_t []){0}, 0x80, 0},
    {0x29, (uint8_t []){0}, 0x80, 0},
    {0, (uint8_t []){0}, 0xff, 0},
};

static esp_err_t backlight_init(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = BL_LEDC_TIMER,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    const ledc_channel_config_t channel = {
        .gpio_num = LCD_GPIO_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = BL_LEDC_CH,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = BL_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "ledc timer");
    return ledc_channel_config(&channel);
}

static void backlight_set(int percent)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BL_LEDC_CH, (1023 * percent) / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BL_LEDC_CH);
}

static bool panel_is_st7789(void)
{
    i2c_master_bus_handle_t bus = (i2c_master_bus_handle_t)get_i2c_bus_handle(0);
    if (bus == NULL) {
        return false;
    }
    return i2c_master_probe(bus, TOUCH_TT21100_ADDR, 50) == ESP_OK;
}

static void build_ui(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "BOX-3 Lab Assistant");
    lv_obj_set_style_text_font(title, FONT_TITLE, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    status_label = lv_label_create(scr);
    lv_label_set_long_mode(status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(status_label, LCD_H_RES - 20);
    lv_obj_set_style_text_font(status_label, FONT_BODY, 0);
    lv_obj_set_style_text_color(status_label, lv_palette_main(LV_PALETTE_AMBER), 0);
    lv_label_set_text(status_label, "Booting...");
    lv_obj_align(status_label, LV_ALIGN_TOP_LEFT, 10, 44);

    transcript_label = lv_label_create(scr);
    lv_label_set_long_mode(transcript_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(transcript_label, LCD_H_RES - 20);
    lv_obj_set_style_text_font(transcript_label, FONT_BODY, 0);
    lv_obj_set_style_text_color(transcript_label, lv_color_white(), 0);
    lv_label_set_text(transcript_label, "");
    lv_obj_align(transcript_label, LV_ALIGN_TOP_LEFT, 10, 96);

    tool_label = lv_label_create(scr);
    lv_label_set_long_mode(tool_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(tool_label, LCD_H_RES - 20);
    lv_obj_set_style_text_font(tool_label, FONT_BODY, 0);
    lv_obj_set_style_text_color(tool_label, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_label_set_text(tool_label, "tool: (none yet)");
    lv_obj_align(tool_label, LV_ALIGN_BOTTOM_LEFT, 10, -10);
}

esp_err_t display_init(void)
{
    ESP_RETURN_ON_ERROR(backlight_init(), TAG, "backlight");

    const spi_bus_config_t bus_cfg = {
        .sclk_io_num = LCD_GPIO_PCLK,
        .mosi_io_num = LCD_GPIO_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = LCD_H_RES * LCD_DRAW_BUF_LINES * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO), TAG, "spi bus");

    esp_lcd_panel_io_handle_t io = NULL;
    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = LCD_GPIO_DC,
        .cs_gpio_num = LCD_GPIO_CS,
        .pclk_hz = LCD_PIXEL_CLK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_cfg, &io), TAG, "panel io");

    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_GPIO_RST,
        .flags.reset_active_high = 1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    if (panel_is_st7789()) {
        ESP_LOGI(TAG, "ST7789 panel variant");
        ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(io, &panel_cfg, &panel), TAG, "st7789");
    } else {
        ESP_LOGI(TAG, "ILI9341 panel variant");
        const ili9341_vendor_config_t vendor = {
            .init_cmds = ili9341_box3_init,
            .init_cmds_size = sizeof(ili9341_box3_init) / sizeof(ili9341_box3_init[0]),
        };
        panel_cfg.vendor_config = (void *)&vendor;
        ESP_RETURN_ON_ERROR(esp_lcd_new_panel_ili9341(io, &panel_cfg, &panel), TAG, "ili9341");
    }
    esp_lcd_panel_reset(panel);
    esp_lcd_panel_init(panel);
    esp_lcd_panel_mirror(panel, true, true);
    esp_lcd_panel_disp_on_off(panel, true);

    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_cfg.task_stack_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT; /* keep internal RAM for WiFi/WebRTC */
    ESP_RETURN_ON_ERROR(lvgl_port_init(&lvgl_cfg), TAG, "lvgl port");
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io,
        .panel_handle = panel,
        .buffer_size = LCD_H_RES * LCD_DRAW_BUF_LINES,
        .double_buffer = false,
        .hres = LCD_H_RES,
        .vres = LCD_V_RES,
        .monochrome = false,
        .rotation = {
            .swap_xy = false,
            .mirror_x = true,
            .mirror_y = true,
        },
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
            .swap_bytes = true,
        },
    };
    if (lvgl_port_add_disp(&disp_cfg) == NULL) {
        ESP_LOGE(TAG, "lvgl_port_add_disp failed");
        return ESP_FAIL;
    }

    lvgl_port_lock(0);
    build_ui();
    lvgl_port_unlock();
    backlight_set(100);
    initialized = true;
    ESP_LOGI(TAG, "Display ready");
    return ESP_OK;
}

void display_set_status(const char *fmt, ...)
{
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ESP_LOGI(TAG, "status: %s", buf);
    if (!initialized) {
        return;
    }
    /* A rejection is followed by a "Disconnected" event a few ms later; keep the reason on screen */
    if (error_sticky && strncmp(buf, "Disconnected", 12) == 0) {
        return;
    }
    error_sticky = false;
    lvgl_port_lock(0);
    lv_label_set_text(status_label, buf);
    lv_obj_set_style_text_color(status_label, lv_palette_main(LV_PALETTE_AMBER), 0);
    lvgl_port_unlock();
}

void display_set_error(const char *fmt, ...)
{
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ESP_LOGE(TAG, "error: %s", buf);
    if (!initialized) {
        return;
    }
    error_sticky = true;
    lvgl_port_lock(0);
    lv_label_set_text(status_label, buf);
    lv_obj_set_style_text_color(status_label, lv_palette_main(LV_PALETTE_RED), 0);
    lvgl_port_unlock();
}

void display_set_listening(bool listening)
{
    if (!initialized) {
        return;
    }
    error_sticky = false;
    lvgl_port_lock(0);
    lv_label_set_text(status_label, listening ? "Listening..." : "Speaking...");
    lv_obj_set_style_text_color(status_label,
                                listening ? lv_palette_main(LV_PALETTE_GREEN)
                                          : lv_palette_main(LV_PALETTE_AMBER), 0);
    lvgl_port_unlock();
}

void display_set_transcript(const char *text)
{
    if (!initialized || text == NULL) {
        return;
    }
    lvgl_port_lock(0);
    lv_label_set_text(transcript_label, text);
    lvgl_port_unlock();
}

void display_set_tool(const char *text, bool ok)
{
    if (!initialized || text == NULL) {
        return;
    }
    lvgl_port_lock(0);
    lv_label_set_text(tool_label, text);
    lv_obj_set_style_text_color(tool_label, ok ? lv_palette_main(LV_PALETTE_GREEN) : lv_palette_main(LV_PALETTE_RED), 0);
    lvgl_port_unlock();
}
