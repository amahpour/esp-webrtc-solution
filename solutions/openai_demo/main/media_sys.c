/* Media system

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include "codec_init.h"
#include "codec_board.h"
#include "av_render.h"
#include "common.h"
#include "settings.h"
#include "media_lib_os.h"
#include "esp_timer.h"
#include "av_render_default.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_enc_default.h"
#include "esp_capture_defaults.h"
#include "esp_capture_sink.h"
#include "esp_log.h"

#define RET_ON_NULL(ptr, v) do {                                \
    if (ptr == NULL) {                                          \
        ESP_LOGE(TAG, "Memory allocate fail on %d", __LINE__);  \
        return v;                                               \
    }                                                           \
} while (0)

#define TAG "MEDIA_SYS"

typedef struct {
    esp_capture_sink_handle_t   capture_handle;
    esp_capture_audio_src_if_t *aud_src;
} capture_system_t;

typedef struct {
    audio_render_handle_t audio_render;
    av_render_handle_t    player;
} player_system_t;

static capture_system_t capture_sys;
static player_system_t  player_sys;

static wake_word_cb_t wake_cb;
static bool            wake_listening;

static void wake_word_detected(int index, void *ctx)
{
    if (wake_cb) {
        wake_cb(index);
    }
}

void media_sys_set_wake_handler(wake_word_cb_t cb)
{
    wake_cb = cb;
}

static int build_capture_system(void)
{
    // Upstream assumes the Korvo-2, where the AEC reference is TDM slot 1. On the ESP32-S3-BOX-3
    // the two mics are ES7210 MIC1/MIC2 and the speaker feedback is MIC3, i.e. slot 2 -- feeding
    // slot 1 as "reference" hands the AEC a second microphone, so nothing gets cancelled and the
    // model answers its own echo. mic_layout "MR" = [mic, reference] over the selected slots.
    esp_capture_audio_aec_src_cfg_t codec_cfg = {
        .record_handle = get_record_handle(),
#if CONFIG_IDF_TARGET_ESP32S3
        .channel = 4,
#ifdef AEC_REF_TDM_SLOT
        .channel_mask = 1 | (1 << AEC_REF_TDM_SLOT),
#else
        .channel_mask = 1 | 2,
#endif
        .mic_layout = "MR",
        .wake_cb = wake_word_detected,
        .wake_ctx = NULL,
#endif
    };
    // capture_sys.aud_src = esp_capture_new_audio_dev_src(&codec_cfg);
    capture_sys.aud_src = esp_capture_new_audio_aec_src(&codec_cfg);
    RET_ON_NULL(capture_sys.aud_src, -1);
    // Create capture system
    esp_capture_cfg_t cfg = {
        .sync_mode = ESP_CAPTURE_SYNC_MODE_AUDIO,
        .audio_src = capture_sys.aud_src,
    };
    esp_capture_open(&cfg, &capture_sys.capture_handle);
    return 0;
}

static int build_player_system()
{
    i2s_render_cfg_t i2s_cfg = {
        .play_handle = get_playback_handle(),
    };
    player_sys.audio_render = av_render_alloc_i2s_render(&i2s_cfg);
    if (player_sys.audio_render == NULL) {
        ESP_LOGE(TAG, "Fail to create audio render");
        return -1;
    }
    esp_codec_dev_set_out_vol(i2s_cfg.play_handle, DEFAULT_PLAYBACK_VOL);
    av_render_cfg_t render_cfg = {
        .audio_render = player_sys.audio_render,
        .audio_raw_fifo_size = 8 * 4096,
        .audio_render_fifo_size = 100 * 1024,
        .allow_drop_data = false,
    };
    player_sys.player = av_render_open(&render_cfg);
    if (player_sys.player == NULL) {
        ESP_LOGE(TAG, "Fail to create player");
        return -1;
    }
    // When support AEC, reference data is from speaker right channel for ES8311 so must output 2 channel
    av_render_audio_frame_info_t aud_info = {
        .sample_rate = 16000,
        .channel = 2,
        .bits_per_sample = 16,
    };
    av_render_set_fixed_frame_info(player_sys.player, &aud_info);

    // Buffer 100ms data to avoid network not stable
    /* Upstream initialises this to 0 and then multiplies, so the threshold is always 0: playback
     * begins with an empty buffer and any network jitter becomes an audible gap. Pre-buffer a
     * little audio instead - it costs PLAYBACK_JITTER_MS of latency and buys smooth speech. */
    uint32_t audio_threshold = PLAYBACK_JITTER_MS;
    audio_threshold *= aud_info.sample_rate * aud_info.channel * (aud_info.bits_per_sample >> 3) / 1000;
    av_render_set_audio_threshold(player_sys.player, audio_threshold);
    return 0;
}

int media_sys_buildup(void)
{
    // Register default audio encoder
    esp_audio_enc_register_default();
    // Register default audio decoder
    esp_audio_dec_register_default();
    // Build capture system
    build_capture_system();
    // Build player system
    build_player_system();
    return 0;
}

int media_sys_get_provider(esp_webrtc_media_provider_t *provide)
{
    provide->capture = capture_sys.capture_handle;
    provide->player = player_sys.player;
    return 0;
}

bool media_sys_audio_busy(void)
{
    return media_sys_audio_pending(NULL);
}

uint32_t media_sys_audio_render_pts(void)
{
    uint32_t pts = 0;
    if (player_sys.player == NULL) {
        return 0;
    }
    av_render_get_render_pts(player_sys.player, &pts);
    return pts;
}

bool media_sys_audio_pending(int *detail)
{
    if (player_sys.player == NULL) {
        return false;
    }
    av_render_fifo_stat_t stat = { 0 };
    if (av_render_get_audio_fifo_level(player_sys.player, &stat) != 0) {
        return false;
    }
    if (detail) {
        detail[0] = stat.data_size;
        detail[1] = stat.q_num;
        detail[2] = stat.render_data_size;
        detail[3] = stat.render_q_num;
        detail[4] = (int)stat.duration;
    }
    return stat.data_size > 0 || stat.render_data_size > 0 || stat.q_num > 0 || stat.render_q_num > 0;
}

static esp_capture_sink_handle_t wake_path;

static void wake_listen_task(void *arg)
{
    while (wake_listening) {
        esp_capture_stream_frame_t frame = {
            .stream_type = ESP_CAPTURE_STREAM_TYPE_AUDIO,
        };
        while (esp_capture_sink_acquire_frame(wake_path, &frame, true) == ESP_CAPTURE_ERR_OK) {
            /* Audio is discarded on purpose: WakeNet has already seen it inside the AFE, and
             * nothing should leave this box until the wake word is spoken. */
            esp_capture_sink_release_frame(wake_path, &frame);
        }
        media_lib_thread_sleep(20);
    }
    media_lib_thread_destroy(NULL);
}

int media_sys_wake_listen(bool enable)
{
    if (enable == wake_listening) {
        return 0;
    }
    if (enable) {
        esp_capture_sink_cfg_t sink_cfg = {
            .audio_info = {
                .format_id = ESP_CAPTURE_FMT_ID_PCM,
                .sample_rate = 16000,
                .channel = 1,
                .bits_per_sample = 16,
            },
        };
        if (esp_capture_sink_setup(capture_sys.capture_handle, 0, &sink_cfg, &wake_path) != ESP_CAPTURE_ERR_OK) {
            ESP_LOGE(TAG, "Fail to set up wake listen sink");
            return -1;
        }
        esp_capture_sink_enable(wake_path, ESP_CAPTURE_RUN_MODE_ALWAYS);
        if (esp_capture_start(capture_sys.capture_handle) != ESP_CAPTURE_ERR_OK) {
            ESP_LOGE(TAG, "Fail to start capture for wake listen");
            return -1;
        }
        wake_listening = true;
        media_lib_thread_handle_t h = NULL;
        media_lib_thread_create(&h, "wake_listen", wake_listen_task, NULL, 4 * 1024, 10, 0);
        ESP_LOGI(TAG, "Listening locally for the wake word (nothing is sent anywhere)");
    } else {
        wake_listening = false;
        media_lib_thread_sleep(60);
        esp_capture_sink_enable(wake_path, ESP_CAPTURE_RUN_MODE_DISABLE);
        esp_capture_stop(capture_sys.capture_handle);
        wake_path = NULL;
        ESP_LOGI(TAG, "Stopped local wake listening");
    }
    return 0;
}

int test_capture_to_player(void)
{
    esp_capture_sink_cfg_t sink_cfg = {
        .audio_info = {
            .format_id = ESP_CAPTURE_FMT_ID_OPUS,
            .sample_rate = 16000,
            .channel = 1,
            .bits_per_sample = 16,
        },
    };
    // Create capture
    esp_capture_sink_handle_t capture_path = NULL;
    esp_capture_sink_setup(capture_sys.capture_handle, 0, &sink_cfg, &capture_path);
    esp_capture_sink_enable(capture_path, ESP_CAPTURE_RUN_MODE_ALWAYS);
    // Create player
    av_render_audio_info_t render_aud_info = {
        .codec = AV_RENDER_AUDIO_CODEC_OPUS,
        .sample_rate = 16000,
        .channel = 1,
    };
    av_render_add_audio_stream(player_sys.player, &render_aud_info);

    uint32_t start_time = (uint32_t)(esp_timer_get_time() / 1000);
    esp_capture_start(capture_sys.capture_handle);
    while ((uint32_t)(esp_timer_get_time() / 1000) < start_time + 20000) {
        media_lib_thread_sleep(30);
        esp_capture_stream_frame_t frame = {
            .stream_type = ESP_CAPTURE_STREAM_TYPE_AUDIO,
        };
        while (esp_capture_sink_acquire_frame(capture_path, &frame, true) == ESP_CAPTURE_ERR_OK) {
            av_render_audio_data_t audio_data = {
                .data = frame.data,
                .size = frame.size,
                .pts = frame.pts,
            };
            av_render_add_audio_data(player_sys.player, &audio_data);
            esp_capture_sink_release_frame(capture_path, &frame);
        }
    }
    esp_capture_stop(capture_sys.capture_handle);
    av_render_reset(player_sys.player);
    return 0;
}
