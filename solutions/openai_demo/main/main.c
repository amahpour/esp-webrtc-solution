/* OpenAI realtime communication test

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <stdlib.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_system.h>
#include <nvs_flash.h>
#include <sys/param.h>
#include "argtable3/argtable3.h"
#include "esp_console.h"
#include "esp_webrtc.h"
#include "media_lib_adapter.h"
#include "media_lib_os.h"
#include "esp_timer.h"
#include "esp_cpu.h"
#include "common.h"
#include "esp_capture.h"
#include "bench_dac.h"
#include "codec_init.h"
#include "esp_codec_dev.h"
#include "display.h"

static int start_chat(int argc, char **argv)
{
    media_sys_wake_listen(false);
    start_webrtc();
    return 0;
}

#define RUN_ASYNC(name, body)           \
    void run_async##name(void *arg)     \
    {                                   \
        body;                           \
        media_lib_thread_destroy(NULL); \
    }                                   \
    media_lib_thread_create_from_scheduler(NULL, #name, run_async##name, NULL);

static SemaphoreHandle_t wake_sem;

/* Opening a session tears down the very capture pipeline that called us and then does TLS, so it
 * cannot run on the audio thread - not even via RUN_ASYNC, which creates a task from that
 * context. The callback only signals; this task does the work on its own stack. */
static void listen_for_wake_word(void);

/* Watches the session so it can never sit open (and billing) unattended, and so a dropped
 * session falls back to local wake-word listening instead of reconnecting silently. */
static void session_watch_task(void *arg)
{
    bool was_up = false;
    while (1) {
        media_lib_thread_sleep(1000);
        bool up = openai_session_active();
        if (up) {
            was_up = true;
#if SESSION_IDLE_TIMEOUT_S
            if (openai_idle_seconds() >= SESSION_IDLE_TIMEOUT_S) {
                printf("Session idle for %d s - closing it\n", openai_idle_seconds());
                display_set_status("Idle - closing session");
                stop_webrtc();
                listen_for_wake_word();
                was_up = false;
            }
#endif
        } else if (was_up) {
            printf("Session ended - back to local wake-word listening\n");
            stop_webrtc();
            listen_for_wake_word();
            was_up = false;
        }
    }
}

static void wake_task(void *arg)
{
    while (1) {
        xSemaphoreTake(wake_sem, portMAX_DELAY);
        printf("Wake word: opening a session\n");
        display_set_status("Wake word heard...");
        media_sys_wake_listen(false);
        start_webrtc();
    }
}

/* Runs on the capture thread - signal and return, do nothing else here. */
static void on_wake_word(int index)
{
    if (wake_sem) {
        xSemaphoreGive(wake_sem);
    }
}

static int wake_cli(int argc, char **argv)
{
    printf("simulating a wake-word detection\n");
    on_wake_word(0);
    return 0;
}

static void listen_for_wake_word(void)
{
#if WAKE_WORD_ENABLED && !AUTO_START_SESSION
    display_set_status("Say \"Hi, ESP\" to start");
    media_sys_wake_listen(true);
#endif
}

static int stop_chat(int argc, char **argv)
{
    RUN_ASYNC(stop, {
        stop_webrtc();
        listen_for_wake_word();
    });
    return 0;
}

static int assert_cli(int argc, char **argv)
{
    *(int *)0 = 0;
    return 0;
}

static int sys_cli(int argc, char **argv)
{
    sys_state_show();
    return 0;
}

static int wifi_cli(int argc, char **argv)
{
    if (argc < 1) {
        return -1;
    }
    char *ssid = argv[1];
    char *password = argc > 2 ? argv[2] : NULL;
    return network_connect_wifi(ssid, password);
}

static int measure_cli(int argc, char **argv)
{
    void measure_enable(bool enable);
    void show_measure(void);
    measure_enable(true);
    media_lib_thread_sleep(1500);
    measure_enable(false);
    return 0;
}

static int rec2play_cli(int argc, char **argv)
{
    test_capture_to_player();
    return 0;
}

static int text_cli(int argc, char **argv)
{
    if (argc > 1) {
        openai_send_text(argv[1]);
    }
    return 0;
}

/* Identify which ES7210 TDM slot carries the AEC reference (the speaker feedback).
 * Plays a 1 kHz tone while recording all four slots and prints per-slot RMS.
 * The mics pick the tone up acoustically; the reference slot is wired to the amplifier,
 * so it shows a far larger jump. Runs entirely on the board - no cloud, no cost. */
static int micscan_cli(int argc, char **argv)
{
    esp_codec_dev_handle_t rec = get_record_handle();
    esp_codec_dev_handle_t play = get_playback_handle();
    if (rec == NULL || play == NULL) {
        printf("micscan: no codec handle (is a session running? type 'stop' first)\n");
        return -1;
    }
    const int sample_rate = 16000;
    const int slots = 4;
    const int frames = 256;
    const int passes = 60; /* ~1 s per pass */
    int16_t *rd = (int16_t *)malloc(frames * slots * sizeof(int16_t));
    int16_t *tone = (int16_t *)malloc(frames * 2 * sizeof(int16_t));
    if (rd == NULL || tone == NULL) {
        free(rd); free(tone);
        printf("micscan: out of memory\n");
        return -1;
    }
    for (int i = 0; i < frames; i++) {
        int16_t v = (int16_t)(12000.0f * sinf(2.0f * (float)M_PI * 1000.0f * (float)i / (float)sample_rate));
        tone[2 * i] = v;
        tone[2 * i + 1] = v;
    }
    esp_codec_dev_sample_info_t rfs = {
        .sample_rate = sample_rate, .bits_per_sample = 16, .channel = slots, .channel_mask = 0,
    };
    int ret = esp_codec_dev_open(rec, &rfs);
    if (ret != 0) {
        free(rd); free(tone);
        printf("micscan: record open failed (%d)\n", ret);
        return -1;
    }
    double quiet[4] = {0}, loud[4] = {0};
    for (int p = 0; p < passes; p++) {
        if (esp_codec_dev_read(rec, rd, frames * slots * sizeof(int16_t)) != 0) {
            break;
        }
        for (int i = 0; i < frames; i++) {
            for (int c = 0; c < slots; c++) {
                double v = rd[i * slots + c];
                quiet[c] += v * v;
            }
        }
    }
    esp_codec_dev_sample_info_t pfs = { .sample_rate = sample_rate, .bits_per_sample = 16, .channel = 2 };
    ret = esp_codec_dev_open(play, &pfs);
    if (ret != 0) {
        printf("micscan: playback open failed (%d) - tone pass skipped\n", ret);
    } else {
        for (int p = 0; p < passes; p++) {
            esp_codec_dev_write(play, tone, frames * 2 * sizeof(int16_t));
            if (esp_codec_dev_read(rec, rd, frames * slots * sizeof(int16_t)) != 0) {
                break;
            }
            for (int i = 0; i < frames; i++) {
                for (int c = 0; c < slots; c++) {
                    double v = rd[i * slots + c];
                    loud[c] += v * v;
                }
            }
        }
        esp_codec_dev_close(play);
    }
    esp_codec_dev_close(rec);
    free(rd); free(tone);

    int best = -1;
    double best_gain = 0;
    printf("\nslot |   quiet RMS |    tone RMS |  gain   (ES7210 MIC%%d)\n");
    printf("-----+-------------+-------------+-------\n");
    double n = (double)(frames * passes);
    for (int c = 0; c < slots; c++) {
        double q = sqrt(quiet[c] / n), l = sqrt(loud[c] / n);
        double gain = (q > 1.0) ? l / q : l;
        printf(" %d   | %11.1f | %11.1f | %6.1f  (MIC%d)\n", c, q, l, gain, c + 1);
        /* Rank by ABSOLUTE response: the mics also hear the speaker acoustically, so their
         * quiet-to-loud ratio can beat the reference. Only the electrically wired reference
         * is several times hotter in absolute terms. */
        if (l > best_gain) {
            best_gain = l;
            best = c;
        }
    }
    if (best >= 0) {
        printf("\n=> strongest response to our own speaker: slot %d."
               " Set AEC_REF_TDM_SLOT to %d in settings.h (currently %d).\n", best, best, AEC_REF_TDM_SLOT);
    } else {
        printf("\n=> no slot responded; is the speaker muted or the volume at 0?\n");
    }
    return 0;
}

static int trace_cli(int argc, char **argv)
{
    bool on = (argc < 2) || (atoi(argv[1]) != 0);
    openai_set_event_trace(on);
    printf("event trace %s\n", on ? "on" : "off");
    return 0;
}

static int vad_cli(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: vad <0.0-1.0>   (lower = more sensitive to your voice)\n");
        return -1;
    }
    return openai_set_vad_threshold(atof(argv[1]));
}

static int vol_cli(int argc, char **argv)
{
    esp_codec_dev_handle_t play = get_playback_handle();
    if (play == NULL) {
        printf("vol: no playback handle\n");
        return -1;
    }
    if (argc > 1) {
        int vol = atoi(argv[1]);
        if (vol < 0 || vol > 100) {
            printf("usage: vol <0-100>\n");
            return -1;
        }
        esp_codec_dev_set_out_vol(play, vol);
    }
    int cur = 0;
    esp_codec_dev_get_out_vol(play, &cur);
    printf("vol: %d\n", cur);
    return 0;
}

/* Real-time audio is unforgiving of a marginal link: every lost packet is a gap you hear. */
static int model_cli(int argc, char **argv)
{
    if (argc > 1) {
        openai_set_model(argv[1]);
    }
    printf("model: %s\n", openai_get_model());
    printf("  try: model gpt-realtime-2   (stock, best audio)\n");
    printf("       model gpt-realtime-mini (cheapest)\n");
    return 0;
}

static int net_cli(int argc, char **argv)
{
    wifi_ap_record_t ap = {0};
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        printf("net: not associated\n");
        return -1;
    }
    const char *quality = ap.rssi > -60 ? "good" : (ap.rssi > -67 ? "usable" : "MARGINAL for realtime audio");
    printf("net: ssid=%s rssi=%d dBm ch=%d — %s\n", (char *)ap.ssid, ap.rssi, ap.primary, quality);
    return 0;
}

static int vref_cli(int argc, char **argv)
{
    if (argc > 1) {
        if (bench_dac_set_vref_mv(atoi(argv[1])) != ESP_OK) {
            printf("usage: vref <1000-5500>   (measured reference in mV)\n");
            return -1;
        }
    }
    printf("vref: %d mV\n", bench_dac_get_vref_mv());
    printf("  to re-measure: dac <ch> %d, read the output, then vref <measured_mV>\n", BENCH_DAC_MAX_MV);
    return 0;
}

static int dac_cli(int argc, char **argv)
{
    if (argc < 3) {
        printf("usage: dac <channel 0-%d> <millivolts 0-%d>\n", BENCH_DAC_CHANNELS - 1, BENCH_DAC_MAX_MV);
        return -1;
    }
    esp_err_t ret = bench_dac_set_mv(atoi(argv[1]), atoi(argv[2]));
    printf("dac: %s\n", ret == ESP_OK ? "ok" : esp_err_to_name(ret));
    return ret == ESP_OK ? 0 : -1;
}

static int init_console()
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "esp>";
    repl_config.task_stack_size = 10 * 1024;
    repl_config.task_priority = 22;
    repl_config.max_cmdline_length = 1024;
    // install console REPL environment
#if CONFIG_ESP_CONSOLE_UART
    esp_console_dev_uart_config_t uart_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&uart_config, &repl_config, &repl));
#elif CONFIG_ESP_CONSOLE_USB_CDC
    esp_console_dev_usb_cdc_config_t cdc_config = ESP_CONSOLE_DEV_CDC_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_cdc(&cdc_config, &repl_config, &repl));
#elif CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    esp_console_dev_usb_serial_jtag_config_t usbjtag_config = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&usbjtag_config, &repl_config, &repl));
#endif
    esp_console_cmd_t cmds[] = {
        {
            .command = "start",
            .help = "Start OpenAI realtime communication\r\n",
            .func = start_chat,
        },
        {
            .command = "stop",
            .help = "Stop chat\n",
            .func = stop_chat,
        },
        {
            .command = "i",
            .help = "Show system status\r\n",
            .func = sys_cli,
        },
        {
            .command = "assert",
            .help = "Assert system\r\n",
            .func = assert_cli,
        },
        {
            .command = "wifi",
            .help = "wifi ssid psw\r\n",
            .func = wifi_cli,
        },
        {
            .command = "m",
            .help = "measure system loading\r\n",
            .func = measure_cli,
        },
        {
            .command = "text",
            .help = "Send text message\r\n",
            .func = text_cli,
        },
        {
            .command = "rec2play",
            .help = "Play recorded voice\r\n",
            .func = rec2play_cli,
        },
        {
            .command = "wake",
            .help = "simulate a wake-word detection (same path as saying the wake word)\r\n",
            .func = wake_cli,
        },
        {
            .command = "micscan",
            .help = "identify the AEC reference TDM slot by playing a tone and measuring all 4 mic slots\r\n",
            .func = micscan_cli,
        },
        {
            .command = "trace",
            .help = "trace [0|1]: log realtime audio events with timing\r\n",
            .func = trace_cli,
        },
        {
            .command = "vad",
            .help = "vad <0.0-1.0>: speech-detection threshold, applied live\r\n",
            .func = vad_cli,
        },
        {
            .command = "vol",
            .help = "vol [0-100]: get or set speaker volume (louder can re-trigger the echo loop)\r\n",
            .func = vol_cli,
        },
        {
            .command = "model",
            .help = "model [name]: show or choose the realtime model, applies on next start\r\n",
            .func = model_cli,
        },
        {
            .command = "net",
            .help = "show WiFi signal strength (weak signal = choppy audio)\r\n",
            .func = net_cli,
        },
        {
            .command = "vref",
            .help = "vref [mv]: show or re-calibrate the DAC reference voltage, no reflash\r\n",
            .func = vref_cli,
        },
        {
            .command = "dac",
            .help = "dac ch mv: set bench DAC channel (0-15) to millivolts (0-3200), same clamps as the voice tool\r\n",
            .func = dac_cli,
        },
    };
    for (int i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    }
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
    return 0;
}

static void thread_scheduler(const char *thread_name, media_lib_thread_cfg_t *schedule_cfg)
{
    if (strcmp(thread_name, "venc_0") == 0) {
        // For H264 may need huge stack if use hardware encoder can set it to small value
        schedule_cfg->priority = 10;
#if CONFIG_IDF_TARGET_ESP32S3
        schedule_cfg->stack_size = 20 * 1024;
#endif
    }
#ifdef WEBRTC_SUPPORT_OPUS
    else if (strcmp(thread_name, "aenc_0") == 0) {
        // For OPUS encoder it need huge stack, when use G711 can set it to small value
        schedule_cfg->stack_size = 40 * 1024;
        schedule_cfg->priority = 10;
        schedule_cfg->core_id = 1;
    } else if (strcmp(thread_name, "buffer_in") == 0) {
        schedule_cfg->stack_size = 6 * 1024;
        schedule_cfg->priority = 10;
        schedule_cfg->core_id = 0;
    }
#endif
    else if (strcmp(thread_name, "AUD_SRC") == 0) {
 #ifdef WEBRTC_SUPPORT_OPUS
        schedule_cfg->stack_size = 40 * 1024;
#endif
        schedule_cfg->priority = 15;
    } else if (strcmp(thread_name, "pc_task") == 0) {
        schedule_cfg->stack_size = 25 * 1024;
        schedule_cfg->priority = 18;
        schedule_cfg->core_id = 1;
    } else if (strcmp(thread_name, "pc_send") == 0) {
        schedule_cfg->stack_size = 4 * 1024;
        schedule_cfg->priority = 15;
        schedule_cfg->core_id = 1;
    } else if (strcmp(thread_name, "Adec") == 0) {
        schedule_cfg->stack_size = 40 * 1024;
        schedule_cfg->priority = 15;
        schedule_cfg->core_id = 0;
    }else if (strcmp(thread_name, "ARender") == 0) {
        schedule_cfg->priority = 20;
    }
    if (strcmp(thread_name, "start") == 0) {
        schedule_cfg->stack_size = 6 * 1024;
    }
}

static void capture_scheduler(const char *name, esp_capture_thread_schedule_cfg_t *schedule_cfg)
{
    media_lib_thread_cfg_t cfg = {
        .stack_size = schedule_cfg->stack_size,
        .priority = schedule_cfg->priority,
        .core_id = schedule_cfg->core_id,
    };
    schedule_cfg->stack_in_ext = true;
    thread_scheduler(name, &cfg);
    schedule_cfg->stack_size = cfg.stack_size;
    schedule_cfg->priority = cfg.priority;
    schedule_cfg->core_id = cfg.core_id;
}

static int network_event_handler(bool connected)
{
    // Run async so that not block wifi event callback
    if (connected) {
#if AUTO_START_SESSION
        display_set_status("WiFi up - calling OpenAI...");
        RUN_ASYNC(start, { start_webrtc(); });
#else
        printf("WiFi connected. No session opened (AUTO_START_SESSION=0).\n"
               "  start     open the OpenAI session now (this bills)\n"
               "  rec2play  20 s local mic test through the AEC pipeline, no cloud, no cost\n");
#if WAKE_WORD_ENABLED
        printf("  or just say \"Hi, ESP\" - listening locally, nothing leaves the board\n");
#else
        display_set_status("WiFi up - type 'start' to open a session");
#endif
        /* Capture + AFE bring-up is far too heavy for the event task's stack. */
        RUN_ASYNC(wakelisten, { listen_for_wake_word(); });
#endif
    } else {
        display_set_status("WiFi disconnected");
        RUN_ASYNC(stop, { stop_webrtc(); });
    }
    return 0;
}

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_INFO);
    media_lib_add_default_adapter();
    esp_capture_set_thread_scheduler(capture_scheduler);
    media_lib_thread_set_schedule_cb(thread_scheduler);
    init_board();
    media_sys_buildup();
    wake_sem = xSemaphoreCreateBinary();
    media_lib_thread_handle_t wake_thread = NULL;
    media_lib_thread_create(&wake_thread, "wake", wake_task, NULL, 8 * 1024, 12, 0);
    media_lib_thread_handle_t watch_thread = NULL;
    media_lib_thread_create(&watch_thread, "sess_watch", session_watch_task, NULL, 6 * 1024, 5, 0);
    media_sys_set_wake_handler(on_wake_word);
    init_console();
    network_init(WIFI_SSID, WIFI_PASSWORD, network_event_handler);
    while (1) {
        media_lib_thread_sleep(2000);
        query_webrtc();
    }
}
