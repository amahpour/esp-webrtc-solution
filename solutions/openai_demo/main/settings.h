/* General settings

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Set used board name, see `codec_board` README.md for more details
 */
#if CONFIG_IDF_TARGET_ESP32P4
#define TEST_BOARD_NAME "ESP32_P4_DEV_V14"
#else
#define TEST_BOARD_NAME "ESP32_S3_BOX_3"
#endif

/**
 * @brief  If defined will use OPUS codec
 */
#define WEBRTC_SUPPORT_OPUS

/**
 * @brief  Whether enable data channel
 */
#define DATA_CHANNEL_ENABLED (true)

/**
 * @brief  Set WiFi SSID
 */
#define WIFI_SSID "XXXX"

/**
 * @brief  Set WiFi password
 */
#define WIFI_PASSWORD "XXXX"

/**
 * @brief  Set default playback volume
 */
#define DEFAULT_PLAYBACK_VOL (85)  /* safe to run loud: MIC_GATE_WHILE_SPEAKING closes the echo path. Tune live with `vol` */

/**
 * @brief  TDM slot carrying the AEC reference (speaker feedback)
 *
 * @note   Measured on the ESP32-S3-BOX-3 with the `micscan` console command: playing a 1 kHz
 *         tone gives slot RMS 852 / 28378 / 797 / 188 for MIC1..MIC4. Slot 1 (ES7210 MIC2) is
 *         33x hotter than the others because it is wired to the amplifier - that is the
 *         reference. Slots 0 and 2 are the two microphones; slot 3 is unused.
 *         Point this at a microphone instead and the AEC subtracts one mic from the other,
 *         cancelling your voice along with the echo. Re-run `micscan` on any new board.
 */
#define AEC_REF_TDM_SLOT (1)

/**
 * @brief  Analog gain applied to the microphone slots only, in dB (ES7210 range 0 - 37.5)
 *
 * @note   The codec defaults every channel to 30 dB when it opens. Boosting only the mic slots
 *         makes quiet or further-away speech clear the server VAD threshold. The AEC reference
 *         slot is deliberately left alone - it is line level and would clip.
 *         Applied after the capture path opens, because opening the codec resets channel gains.
 */
#define MIC_INPUT_GAIN_DB (36)

/**
 * @brief  Mute the microphone while the assistant is speaking (half duplex)
 *
 * @note   The on-chip AEC runs in low-cost mode with non-linear processing off, so at any
 *         useful speaker volume enough echo survives for the server VAD to treat it as
 *         speech - the model then answers itself forever. Gating the mic during playback
 *         removes the path entirely, at the cost of barge-in. Set to 0 to rely on AEC alone.
 */
#define MIC_GATE_WHILE_SPEAKING (1)

/**
 * @brief  How often to check whether the speaker has finished (ms)
 *
 * @note   A fixed delay cannot work: the render FIFO is 100 KB, over 3 s of 16 kHz audio, so
 *         `response.done` can precede the last spoken word by seconds. The mic re-opens once
 *         `media_sys_audio_busy()` reports the pipeline drained (3 consecutive idle polls),
 *         with a 12 s failsafe.
 */
#define MIC_GATE_POLL_MS (50)

/**
 * @brief  Quiet time required after the render FIFO empties before the mic re-opens (ms)
 *
 * @note   Measured from `output_audio_buffer.stopped`, the server's signal that the speaker has
 *         actually finished. That is the SERVER's view of its output buffer, so the local jitter
 *         buffer, render FIFO and codec are still a few hundred ms behind it. Fixed and small:
 *         it does not scale with how long the box spoke. Keep it above PLAYBACK_JITTER_MS.
 *         Lower = snappier, more echo risk.
 */
#define MIC_GATE_DRAIN_MS (300)

/**
 * @brief  Audio buffered before playback starts, in ms (0 = play as it arrives)
 *
 * @note   KEEP THIS 0 unless you have measured a reason not to. av_render only re-buffers when a
 *         threshold is set, and its logic (av_render.c ~line 693) re-arms whenever the queue dips
 *         below 3 frames, then stalls until it has re-accumulated the WHOLE threshold. So a
 *         non-zero value converts every small dip into a stutter of this length - 400 ms here
 *         produced six audible breaks in one reply. With 0 the audio simply plays as it arrives.
 *         Genuine choppiness is far more likely to be a weak WiFi link: check `net` first,
 *         since below about -67 dBm packets are actually lost and no buffering can recreate them.
 */
#define PLAYBACK_JITTER_MS (0)

/**
 * @brief  Open the (billed) OpenAI session automatically once WiFi is up
 *
 * @note   Set to 1 for filming, so the box is live straight out of a reboot.
 *         Set to 0 while developing: a realtime session bills for as long as it is open,
 *         and every reflash would otherwise start one. Use the `start` command instead.
 */
#define AUTO_START_SESSION (0)

/**
 * @brief  Wait for the wake word ("Hi, ESP") before opening a session
 *
 * @note   While idle the capture pipeline runs locally so WakeNet can listen, and every frame is
 *         discarded - no audio leaves the board and nothing is billed until you address it.
 *         Requires the WakeNet model in the `model` partition (see sdkconfig.defaults).
 *         Ignored when AUTO_START_SESSION is 1.
 */
#define WAKE_WORD_ENABLED (1)

/**
 * @brief  Close the session after this many seconds with nothing said (0 disables)
 *
 * @note   A realtime session bills for as long as it is open, so it must not stay open just
 *         because nobody closed it. On timeout the box drops back to local wake-word listening.
 */
#define SESSION_IDLE_TIMEOUT_S (120)

#ifdef __cplusplus
}
#endif
