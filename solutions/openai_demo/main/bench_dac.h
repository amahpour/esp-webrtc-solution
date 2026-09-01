/* Bench DAC: two TI DAC7578 on the ESP32-S3-BOX-3 dock I2C bus

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BENCH_DAC_COUNT        (2)                                     /*!< Two DAC7578 breakouts on one bus */
#define BENCH_DAC_CH_PER_DEV   (8)
#define BENCH_DAC_CHANNELS     (BENCH_DAC_COUNT * BENCH_DAC_CH_PER_DEV) /*!< 0-7 on DAC A, 8-15 on DAC B */
/* Output = code/4095 * Vref, so this constant must match the board's ACTUAL reference, which is
 * whatever supplies VCC through the Adafruit DACx578 VREF jumper - never assume a round 3300.
 * History on this bench (2026-08-28): 3383 mV on the BOX-3's 3V3 rail, which then sagged; now
 * 3229 mV on an external supply. Re-measure whenever the supply changes:
 *   dac <ch> 3200   then  Vref = measured * 4095 / code_shown_in_the_log */
#define BENCH_DAC_VREF_MV      (3420)

/* Firmware clamps. The model decides what to do; the firmware decides what is allowed.
 * Requests outside these limits are refused (ESP_ERR_INVALID_ARG), never silently clipped. */
#define BENCH_DAC_MAX_MV       (3200)   /*!< Highest voltage the model may request; must stay <= BENCH_DAC_VREF_MV */
#define BENCH_DAC_CHANNEL_MASK (0xFFFF) /*!< Bit n set = channel n may be driven */

/**
 * @brief  Bring up the dock I2C bus (SCL GPIO40 / SDA GPIO41) and probe both DACs
 */
esp_err_t bench_dac_init(void);

/**
 * @brief  Last value written to a channel, in millivolts
 *
 * @param[in]  channel  0..BENCH_DAC_CHANNELS-1
 *
 * @return  Millivolts, or -1 if the channel is out of range
 */
int bench_dac_get_mv(int channel);

/**
 * @brief  Current reference voltage the conversion uses, in mV
 */
int bench_dac_get_vref_mv(void);

/**
 * @brief  Re-calibrate the reference at runtime, no reflash needed
 *
 * @note   Output = code/4095 * Vref, so this must match the board's actual reference. Measure it
 *         by driving full scale and reading the output. Bench supplies drift and get adjusted;
 *         this has moved between 3.23 V and 3.42 V on the same rig.
 *
 * @param[in]  mv  Measured reference in millivolts (1000-5500)
 *
 * @return  ESP_OK, or ESP_ERR_INVALID_ARG if out of range
 */
esp_err_t bench_dac_set_vref_mv(int mv);

/**
 * @brief  Set one DAC output channel and update it immediately
 *
 * @param[in]  channel     0..BENCH_DAC_CHANNELS-1
 * @param[in]  millivolts  0..BENCH_DAC_MAX_MV
 *
 * @return
 *      - ESP_OK               Output updated
 *      - ESP_ERR_INVALID_ARG  Refused by firmware clamp
 *      - Others               I2C failure
 */
esp_err_t bench_dac_set_mv(int channel, int millivolts);

#ifdef __cplusplus
}
#endif
