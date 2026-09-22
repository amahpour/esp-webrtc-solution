/* Common header

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "settings.h"
#include "media_sys.h"
#include "network.h"
#include "sys_state.h"
#include "esp_webrtc.h"

#define OPENAI_DEFAULT_MODEL "gpt-realtime-mini"  /* cheapest realtime tier; gpt-realtime-2 for top quality */
#define OPENAI_DEFAULT_VOICE "alloy"

/**
 * @brief  Initialize board
 */
void init_board(void);

/**
 * @brief  Wake-word callback type
 */
typedef void (*wake_word_cb_t)(int index);

/**
 * @brief  Register the handler invoked when WakeNet hears the wake word
 */
void media_sys_set_wake_handler(wake_word_cb_t cb);

/**
 * @brief  Run the capture pipeline locally so WakeNet can listen with no session open
 *
 * @note   Frames are discarded; no audio leaves the device until the wake word starts a session.
 *
 * @param[in]  enable  true to listen locally, false to release the capture path
 */
int media_sys_wake_listen(bool enable);

/**
 * @brief  Whether the speaker still has audio queued
 *
 * @note   The render FIFO holds seconds of audio, so `response.done` does NOT mean the box has
 *         stopped talking. Used to decide when it is safe to un-mute the microphone.
 *
 * @return  true if audio is still queued or rendering
 */
bool media_sys_audio_busy(void);

/**
 * @brief  Same as `media_sys_audio_busy`, but also reports the raw FIFO counters
 *
 * @param[out]  detail  Optional array of 5 ints: data_size, q_num, render_data_size,
 *                      render_q_num, duration
 */
bool media_sys_audio_pending(int *detail);

/**
 * @brief  Playback timestamp of the audio renderer, in ms
 *
 * @note   Advances only while audio is actually being pushed to the speaker, so a value that
 *         stops changing means the speaker has genuinely gone quiet. The FIFO counters cannot
 *         be used for this: the renderer keeps ring buffers allocated, so they never read zero.
 *
 * @return  Current render PTS, or 0 if there is no player
 */
uint32_t media_sys_audio_render_pts(void);

/**
 * @brief  OpenAI signaling configuration
 *
 * @note   Details see: https://platform.openai.com/docs/guides/realtime-webrtc
 */
typedef struct {
   char *token; /*!< OpenAI token */
   char *model; /*!< Realtime model to use */
   char *voice; /*!< Voice to select */
} openai_signaling_cfg_t;

/**
 * @brief  Get OpenAI signaling implementation
 *
 * @return
 *      - NULL    Not enough memory
 *      - Others  OpenAI signaling implementation
 */
const esp_peer_signaling_impl_t *esp_signaling_get_openai_signaling(void);

/**
 * @brief  Start WebRTC
 *
 * @return
 *      - 0       On success
 *      - Others  Fail to start
 */
int start_webrtc(void);

/**
 * @brief  Set the server-VAD threshold and re-apply the session config
 *
 * @param[in]  threshold  0.0 (most sensitive) to 1.0 (least sensitive)
 *
 * @return
 *      - 0       On success
 *      - Others  Fail to apply
 */
int openai_set_vad_threshold(double threshold);

/**
 * @brief  Log realtime audio/response events with timing, to see when audio really ends
 */
void openai_set_event_trace(bool on);

/**
 * @brief  Send text to OpenAI server
 *
 * @param[in]  text  Text to be sent
 *
 * @return
 *      - 0       On success
 *      - Others  Fail to start
 */
int openai_send_text(char *text);

/**
 * @brief  Whether a realtime session is currently open (and therefore billing)
 */
bool openai_session_active(void);

/**
 * @brief  Realtime model used for the next session
 */
const char *openai_get_model(void);

/**
 * @brief  Choose the realtime model (applies on the next `start`)
 *
 * @note   `gpt-realtime-2` is what the stock demo uses and sounds best; `gpt-realtime-mini` is
 *         markedly cheaper. Switchable at runtime so the two can be compared back to back.
 */
int openai_set_model(const char *name);

/**
 * @brief  Seconds since the last thing the box heard or did in this session
 */
int openai_idle_seconds(void);

/**
 * @brief  Query WebRTC status
 */
void query_webrtc(void);

/**
 * @brief  Start WebRTC
 *
 * @param[in]  url  Signaling URL
 *
 * @return
 *      - 0       On success
 *      - Others  Fail to start
 */
int stop_webrtc(void);

#ifdef __cplusplus
}
#endif
