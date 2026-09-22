/* Status display on the ESP32-S3-BOX-3 LCD

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Bring up the BOX-3 LCD (SPI3, ILI9341/ST7789) with LVGL and show the status screen
 *
 * @note   Uses the codec I2C bus (already initialised by codec_board) only to detect the panel variant.
 */
esp_err_t display_init(void);

/**
 * @brief  Top line: connection / session state (printf-style)
 */
void display_set_status(const char *fmt, ...);

/**
 * @brief  Top line, red and sticky: an error the user must act on. Stays until the next fresh
 *         connection attempt (any status other than a "Disconnected" notice replaces it).
 */
void display_set_error(const char *fmt, ...);

/**
 * @brief  Live microphone state on the top line: green "Listening..." vs amber "Speaking..."
 *
 * @param[in]  listening  true when the mic is open, false while the box is talking
 */
void display_set_listening(bool listening);

/**
 * @brief  Middle: last transcript line heard from / spoken by the assistant
 */
void display_set_transcript(const char *text);

/**
 * @brief  Bottom: last tool call and its outcome (green when ok, red when refused/failed)
 */
void display_set_tool(const char *text, bool ok);

#ifdef __cplusplus
}
#endif
