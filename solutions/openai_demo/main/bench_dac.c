/* Bench DAC: two TI DAC7578 (Adafruit DACx578 breakouts) on the ESP32-S3-BOX-3 dock I2C bus

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.

   Wiring via ESP32-S3-BOX-3-BREAD:
     SCL = GPIO40, SDA = GPIO41  -- Espressif's designated dock I2C (BSP_I2C_DOCK_*),
                                    separate from the internal codec/touch bus on GPIO8/18
     3V3 OUT -> VCC, GND -> GND  -- the BOX side has no pull-ups; each breakout carries 10K
     DAC A: AD0 floating (default) -> 0x4C, channels 0-7
     DAC B: AD0 pad bridged        -> 0x48, channels 8-15

   Frame (TI SBAS496 Tables 14/21): [C3..C0 A3..A0] [D11..D4] [D3..D0 xxxx]
     C = 0011: write input register n and update DAC n. No LDAC needed
     (the LDAC pin has a known silicon bug on this part).
*/

#include "bench_dac.h"
#include "driver/i2c_master.h"
#include "esp_log.h"

#define TAG "BENCH_DAC"

#define BENCH_I2C_PORT        (I2C_NUM_1)
#define BENCH_I2C_SCL_IO      (40)
#define BENCH_I2C_SDA_IO      (41)
#define BENCH_I2C_FREQ_HZ     (400000)
#define BENCH_I2C_TIMEOUT_MS  (50)

#define DAC7578_CMD_WRITE_UPDATE_CH (0x3)
#define DAC7578_FULL_SCALE          (4095)

static const uint8_t           dac_addr[BENCH_DAC_COUNT] = { 0x4C, 0x48 };
static i2c_master_bus_handle_t bus;
static i2c_master_dev_handle_t dev[BENCH_DAC_COUNT];
static int                     last_mv[BENCH_DAC_CHANNELS];
static int                     vref_mv = BENCH_DAC_VREF_MV;

esp_err_t bench_dac_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = BENCH_I2C_PORT,
        .sda_io_num = BENCH_I2C_SDA_IO,
        .scl_io_num = BENCH_I2C_SCL_IO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Bus on SCL %d / SDA %d failed: %s", BENCH_I2C_SCL_IO, BENCH_I2C_SDA_IO, esp_err_to_name(ret));
        return ret;
    }
    for (int i = 0; i < BENCH_DAC_COUNT; i++) {
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = dac_addr[i],
            .scl_speed_hz = BENCH_I2C_FREQ_HZ,
        };
        ret = i2c_master_bus_add_device(bus, &dev_cfg, &dev[i]);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Add DAC %c @0x%02X failed: %s", 'A' + i, dac_addr[i], esp_err_to_name(ret));
            return ret;
        }
        esp_err_t probe = i2c_master_probe(bus, dac_addr[i], BENCH_I2C_TIMEOUT_MS);
        ESP_LOGI(TAG, "DAC %c @0x%02X (channels %d-%d): %s", 'A' + i, dac_addr[i],
                 i * BENCH_DAC_CH_PER_DEV, i * BENCH_DAC_CH_PER_DEV + BENCH_DAC_CH_PER_DEV - 1,
                 probe == ESP_OK ? "present" : "not responding");
    }
    return ESP_OK;
}

int bench_dac_get_vref_mv(void)
{
    return vref_mv;
}

esp_err_t bench_dac_set_vref_mv(int mv)
{
    if (mv < 1000 || mv > 5500) {
        return ESP_ERR_INVALID_ARG;
    }
    vref_mv = mv;
    ESP_LOGI(TAG, "Vref now %d mV (max requestable %d mV)", vref_mv,
             vref_mv < BENCH_DAC_MAX_MV ? vref_mv : BENCH_DAC_MAX_MV);
    return ESP_OK;
}

int bench_dac_get_mv(int channel)
{
    if (channel < 0 || channel >= BENCH_DAC_CHANNELS) {
        return -1;
    }
    return last_mv[channel];
}

esp_err_t bench_dac_set_mv(int channel, int millivolts)
{
    // Clamps first: refuse, don't clip
    if (channel < 0 || channel >= BENCH_DAC_CHANNELS || ((BENCH_DAC_CHANNEL_MASK >> channel) & 1) == 0) {
        ESP_LOGW(TAG, "Refused: channel %d is not allowed", channel);
        return ESP_ERR_INVALID_ARG;
    }
    int max_mv = (vref_mv < BENCH_DAC_MAX_MV) ? vref_mv : BENCH_DAC_MAX_MV;
    if (millivolts < 0 || millivolts > max_mv) {
        ESP_LOGW(TAG, "Refused: %d mV is outside 0-%d mV", millivolts, max_mv);
        return ESP_ERR_INVALID_ARG;
    }
    i2c_master_dev_handle_t dac = dev[channel / BENCH_DAC_CH_PER_DEV];
    if (dac == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    unsigned code = ((unsigned)millivolts * DAC7578_FULL_SCALE + vref_mv / 2) / vref_mv;
    if (code > DAC7578_FULL_SCALE) {
        /* Only reachable if BENCH_DAC_MAX_MV is set above Vref. Without this the code would not
         * fit in 12 bits and the byte packing below would wrap to a wildly wrong voltage. */
        code = DAC7578_FULL_SCALE;
    }
    uint8_t frame[3] = {
        (uint8_t)((DAC7578_CMD_WRITE_UPDATE_CH << 4) | (channel % BENCH_DAC_CH_PER_DEV)),
        (uint8_t)(code >> 4),
        (uint8_t)((code & 0x0F) << 4),
    };
    esp_err_t ret = i2c_master_transmit(dac, frame, sizeof(frame), BENCH_I2C_TIMEOUT_MS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Channel %d: I2C write failed: %s", channel, esp_err_to_name(ret));
        return ret;
    }
    last_mv[channel] = millivolts;
    ESP_LOGI(TAG, "Channel %d -> %d mV (code %u, frame %02X %02X %02X)", channel, millivolts, code,
             frame[0], frame[1], frame[2]);
    return ESP_OK;
}
