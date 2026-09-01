/* OpenAI realtime communication Demo code

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include "esp_webrtc.h"
#include "media_lib_os.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_webrtc_defaults.h"
#include "esp_peer_default.h"
#include "common.h"
#include <stdlib.h>
#include <strings.h>
#include <cJSON.h>
#include "bench_dac.h"
#include "display.h"
#include "codec_init.h"
#include "esp_codec_dev.h"
#include "esp_timer.h"

#define TAG "OPENAI_APP"

#define ELEMS(a) (sizeof(a) / sizeof(a[0]))

typedef struct attribute_t attribute_t;
typedef struct class_t     class_t;
typedef enum {
    ATTRIBUTE_TYPE_NONE,
    ATTRIBUTE_TYPE_BOOL,
    ATTRIBUTE_TYPE_INT,
    ATTRIBUTE_TYPE_PARENT,
} attribute_type_t;

struct attribute_t {
    char            *name;
    char            *desc;
    attribute_type_t type;
    union {
        bool         b_state;
        int          i_value;
        attribute_t *attr_list;
    };
    int  attr_num;
    bool required;
    int (*control)(attribute_t *attr);
};

struct class_t {
    char        *name;
    char        *desc;
    attribute_t *attr_list;
    int          attr_num;
    int        (*control)(class_t *cls, char *out, int out_len); /*!< Runs after all arguments are parsed, fills the tool output JSON */
    class_t     *next;
};

static esp_webrtc_handle_t webrtc  = NULL;
static class_t            *classes = NULL;
static char                last_function_call_id[64] = {0};
/* Tunable at runtime with the `vad` console command: too high and it cannot hear you,
 * too low and it answers room noise (or its own speaker, if AEC is misconfigured). */
static double              vad_threshold = 0.45;
static esp_timer_handle_t  mic_gate_timer;
static int64_t             gate_done_us;
static bool                event_trace;
static bool                mic_muted;
static bool                session_up;
static char                model_name[48] = OPENAI_DEFAULT_MODEL;

const char *openai_get_model(void)
{
    return model_name;
}

int openai_set_model(const char *name)
{
    if (name == NULL || name[0] == 0) {
        return -1;
    }
    snprintf(model_name, sizeof(model_name), "%s", name);
    printf("model set to %s (takes effect on the next `start`)\n", model_name);
    return 0;
}
static int64_t             last_activity_us;

static void note_activity(void)
{
    last_activity_us = esp_timer_get_time();
}

bool openai_session_active(void)
{
    return session_up;
}

int openai_idle_seconds(void)
{
    if (!session_up || last_activity_us == 0) {
        return 0;
    }
    return (int)((esp_timer_get_time() - last_activity_us) / 1000000);
}

/* Half duplex: the mic is deaf while the box talks, so its own voice cannot reach the VAD. */
static void set_mic_muted(bool mute)
{
    esp_codec_dev_handle_t rec = get_record_handle();
    if (rec == NULL || mute == mic_muted) {
        return;
    }
    if (esp_codec_dev_set_in_mute(rec, mute) == 0) {
        mic_muted = mute;
        printf("MIC %s\n", mute ? "muted (assistant speaking)" : "live");
        display_set_listening(!mute);
    }
}

/* response.done only means the model stopped generating; seconds of audio may still be queued
 * in the render FIFO. Poll until the speaker is genuinely idle, then listen again. */
static void mic_gate_reopen(void *arg)
{
    esp_timer_stop(mic_gate_timer);
    printf("gate: mic live %lld ms after the speaker stopped\n",
           (esp_timer_get_time() - gate_done_us) / 1000);
    set_mic_muted(false);
}

/* The server tells us exactly when the speaker starts and stops: `output_audio_buffer.started`
 * and `output_audio_buffer.stopped`. Everything else is a poor proxy - `response.done` fires when
 * the model stops *generating*, which was up to 3 s before the audio actually finished playing,
 * and the render FIFO never empties because the WebRTC track streams continuously. */
static void mic_gate(bool speaking)
{
#if MIC_GATE_WHILE_SPEAKING
    if (mic_gate_timer == NULL) {
        const esp_timer_create_args_t args = { .callback = mic_gate_reopen, .name = "mic_gate" };
        if (esp_timer_create(&args, &mic_gate_timer) != ESP_OK) {
            return;
        }
    }
    esp_timer_stop(mic_gate_timer);
    if (speaking) {
        set_mic_muted(true);
    } else {
        gate_done_us = esp_timer_get_time();
        esp_timer_start_once(mic_gate_timer, MIC_GATE_DRAIN_MS * 1000);
    }
#endif
}

/* Argument slots, filled by match_and_execute() before the class control runs */
static attribute_t bench_attrs[] = {
    {
        .name = "channel",
        .desc = "DAC output channel, integer 0-15 (0-7 on DAC A, 8-15 on DAC B)",
        .type = ATTRIBUTE_TYPE_INT,
        .required = true,
    },
    {
        .name = "millivolts",
        .desc = "Target output voltage in millivolts, integer 0-3200",
        .type = ATTRIBUTE_TYPE_INT,
        .required = true,
    },
};

/* The model decides what to do. The firmware decides what is allowed. */
static int set_channel_voltage(class_t *cls, char *out, int out_len)
{
    int channel = bench_attrs[0].i_value;
    int millivolts = bench_attrs[1].i_value;
    esp_err_t ret = bench_dac_set_mv(channel, millivolts);
    char shown[96];
    snprintf(shown, sizeof(shown), "set_channel_voltage(ch %d, %d mV)\n%s", channel, millivolts,
             ret == ESP_OK ? "OK - written to DAC" : ret == ESP_ERR_INVALID_ARG ? "REFUSED by firmware clamp" : "FAILED - I2C error");
    display_set_tool(shown, ret == ESP_OK);
    if (ret == ESP_ERR_INVALID_ARG) {
        snprintf(out, out_len,
                 "{\"ok\":false,\"error\":\"refused by firmware clamp: channel must be 0-%d and millivolts 0-%d\"}",
                 BENCH_DAC_CHANNELS - 1, BENCH_DAC_MAX_MV);
        return -1;
    }
    if (ret != ESP_OK) {
        snprintf(out, out_len, "{\"ok\":false,\"error\":\"I2C write failed (%s)\"}", esp_err_to_name(ret));
        return -1;
    }
    snprintf(out, out_len, "{\"ok\":true,\"channel\":%d,\"millivolts\":%d}", channel, millivolts);
    return 0;
}

/* Real speaker volume, not a stub: drives the ES8311 through esp_codec_dev. */
static attribute_t volume_attrs[] = {
    {
        .name = "volume",
        .desc = "Speaker volume, integer 0-100",
        .type = ATTRIBUTE_TYPE_INT,
        .required = true,
    },
};

static int set_speaker_volume(class_t *cls, char *out, int out_len)
{
    int vol = volume_attrs[0].i_value;
    if (vol < 0 || vol > 100) {
        snprintf(out, out_len, "{\"ok\":false,\"error\":\"volume must be 0-100\"}");
        return -1;
    }
    esp_codec_dev_handle_t play = get_playback_handle();
    if (play == NULL || esp_codec_dev_set_out_vol(play, vol) != 0) {
        snprintf(out, out_len, "{\"ok\":false,\"error\":\"could not set volume\"}");
        return -1;
    }
    char shown[64];
    snprintf(shown, sizeof(shown), "set_speaker_volume(%d)\nOK", vol);
    display_set_tool(shown, true);
    printf("Volume set to %d\n", vol);
    snprintf(out, out_len, "{\"ok\":true,\"volume\":%d}", vol);
    return 0;
}

/* Read back what a bench channel is currently set to. */
static attribute_t read_attrs[] = {
    {
        .name = "channel",
        .desc = "DAC output channel, integer 0-15",
        .type = ATTRIBUTE_TYPE_INT,
        .required = true,
    },
};

static int get_channel_voltage(class_t *cls, char *out, int out_len)
{
    int ch = read_attrs[0].i_value;
    int mv = bench_dac_get_mv(ch);
    if (mv < 0) {
        snprintf(out, out_len, "{\"ok\":false,\"error\":\"channel must be 0-%d\"}", BENCH_DAC_CHANNELS - 1);
        return -1;
    }
    snprintf(out, out_len, "{\"ok\":true,\"channel\":%d,\"millivolts\":%d}", ch, mv);
    return 0;
}

static class_t *build_bench_class(void)
{
    class_t *bench = (class_t *)calloc(1, sizeof(class_t));
    if (bench == NULL) {
        return NULL;
    }
    bench->name = "set_channel_voltage";
    bench->desc = "Set one bench DAC output channel to a voltage. The output steps immediately; a scope on that channel shows it.";
    bench->attr_list = bench_attrs;
    bench->attr_num = ELEMS(bench_attrs);
    bench->control = set_channel_voltage;
    return bench;
}

static class_t *build_volume_class(void)
{
    class_t *vol = (class_t *)calloc(1, sizeof(class_t));
    if (vol == NULL) {
        return NULL;
    }
    vol->name = "set_speaker_volume";
    vol->desc = "Set how loud this device speaks, 0 to 100.";
    vol->attr_list = volume_attrs;
    vol->attr_num = ELEMS(volume_attrs);
    vol->control = set_speaker_volume;
    return vol;
}

static class_t *build_read_class(void)
{
    class_t *rd = (class_t *)calloc(1, sizeof(class_t));
    if (rd == NULL) {
        return NULL;
    }
    rd->name = "get_channel_voltage";
    rd->desc = "Report the voltage a bench DAC channel is currently set to.";
    rd->attr_list = read_attrs;
    rd->attr_num = ELEMS(read_attrs);
    rd->control = get_channel_voltage;
    return rd;
}

static void add_class(class_t *cls)
{
    if (cls == NULL) {
        return;
    }
    if (classes == NULL) {
        classes = cls;
    } else {
        cls->next = classes;
        classes = cls;
    }
}

static int build_classes(void)
{
    static bool build_once = false;
    if (build_once) {
        return 0;
    }
    add_class(build_bench_class());
    add_class(build_volume_class());
    add_class(build_read_class());
    build_once = true;
    return 0;
}

static char *get_attr_type(attribute_type_t type)
{
    if (type == ATTRIBUTE_TYPE_BOOL) {
        return "boolean";
    }
    if (type == ATTRIBUTE_TYPE_INT) {
        return "integer";
    }
    if (type == ATTRIBUTE_TYPE_PARENT) {
        return "object";
    }
    return "";
}

static int add_parent_attribute(cJSON *parent, attribute_t *attr)
{
    cJSON *properties = cJSON_CreateObject();
    cJSON_AddItemToObject(parent, "properties", properties);
    int require_num = 0;
    for (int i = 0; i < attr->attr_num; i++) {
        attribute_t *sub_attr = &attr->attr_list[i];
        cJSON *prop = cJSON_CreateObject();
        cJSON_AddItemToObject(properties, sub_attr->name, prop);
        cJSON_AddStringToObject(prop, "type", get_attr_type(sub_attr->type));
        cJSON_AddStringToObject(prop, "description", sub_attr->desc);
        if (sub_attr->type == ATTRIBUTE_TYPE_PARENT) {
            add_parent_attribute(prop, sub_attr);
        }
        if (sub_attr->required) {
            require_num++;
        }
    }
    if (require_num) {
        cJSON *requires = cJSON_CreateArray();
        for (int i = 0; i < attr->attr_num; i++) {
            attribute_t *sub_attr = &attr->attr_list[i];
            if (sub_attr->required) {
                cJSON_AddItemToArray(requires, cJSON_CreateString(sub_attr->name));
            }
        }
        cJSON_AddItemToObject(parent, "required", requires);
    }
    return 0;
}

static int send_datachannel_json(cJSON *root)
{
    if (webrtc == NULL || root == NULL) {
        return -1;
    }
    char *json_string = cJSON_PrintUnformatted(root);
    if (json_string == NULL) {
        return -1;
    }
    printf("Begin to send json:%s\n", json_string);
    int ret = esp_webrtc_send_custom_data(webrtc, ESP_WEBRTC_CUSTOM_DATA_VIA_DATA_CHANNEL,
                                          (uint8_t *)json_string, strlen(json_string));
    free(json_string);
    return ret;
}

/*
 * OpenAI Realtime GA no longer accepts beta fields like session.modalities /
 * response.modalities. Use session.type=realtime and output_modalities instead.
 * Docs: https://developers.openai.com/api/docs/guides/realtime-conversations
 */
static int send_function_desc(void)
{
    if (classes == NULL || webrtc == NULL) {
        return 0;
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "session.update");
    cJSON *session = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "session", session);

    cJSON_AddStringToObject(session, "type", "realtime");
    cJSON *modalities = cJSON_CreateArray();
    /* GA: ["audio"] gives spoken audio + transcript; ["audio","text"] is rejected */
    cJSON_AddItemToArray(modalities, cJSON_CreateString("audio"));
    cJSON_AddItemToObject(session, "output_modalities", modalities);
    cJSON_AddStringToObject(session, "instructions",
                            "You are a bench lab assistant built into an ESP32-S3-BOX-3. Your tools: "
                            "set_channel_voltage (drive a DAC output channel 0-15 to 0-3200 mV), "
                            "get_channel_voltage (read one back), and "
                            "set_speaker_volume (how loud you speak, 0-100). "
                            "Call a tool whenever the user asks for something a tool covers - including "
                            "loudness requests like 'turn it up', which mean set_speaker_volume. "
                            "NEVER guess a channel number. If you did not clearly hear which channel, ask "
                            "'which channel?' and do not call the tool. Do not default to channel 0. "
                            "Volts map to millivolts: 'two volts' is 2000, 'one point eight volts' is 1800. "
                            "You can also just talk: answer questions about the bench, electronics and this "
                            "hardware normally. If asked for something you have no tool for, say so in one "
                            "short sentence instead of pretending. "
                            "If a tool reports an error, say what failed - never claim something changed when it did not. "
                            "Keep replies to one or two short sentences unless asked for detail.");
    cJSON_AddStringToObject(session, "tool_choice", "auto");

    /* The BOX-3's mic hears its own speaker. Without an explicit turn_detection block the model
     * answers its own echo and never yields the floor. Raise the VAD threshold, require a longer
     * silence before it decides you stopped talking, and allow barge-in so speech cuts it off. */
    cJSON *audio = cJSON_CreateObject();
    cJSON *input = cJSON_CreateObject();
    cJSON *turn = cJSON_CreateObject();
    cJSON_AddStringToObject(turn, "type", "server_vad");
    cJSON_AddNumberToObject(turn, "threshold", vad_threshold);
    cJSON_AddNumberToObject(turn, "prefix_padding_ms", 600);
    cJSON_AddNumberToObject(turn, "silence_duration_ms", 700);
    cJSON_AddBoolToObject(turn, "create_response", true);
    cJSON_AddBoolToObject(turn, "interrupt_response", true);
    cJSON_AddItemToObject(input, "turn_detection", turn);
    cJSON *noise = cJSON_CreateObject();
    cJSON_AddStringToObject(noise, "type", "near_field");
    cJSON_AddItemToObject(input, "noise_reduction", noise);
    /* Transcribe the uplink audio: if this stays silent while you talk, the problem is the mic
     * path (AEC reference / VAD), not the model. */
    cJSON *transcription = cJSON_CreateObject();
    cJSON_AddStringToObject(transcription, "model", "gpt-4o-mini-transcribe");
    cJSON_AddItemToObject(input, "transcription", transcription);
    cJSON_AddItemToObject(audio, "input", input);
    cJSON_AddItemToObject(session, "audio", audio);

    cJSON *tools = cJSON_CreateArray();
    cJSON_AddItemToObject(session, "tools", tools);

    class_t *iter = classes;
    while (iter) {
        cJSON *tool = cJSON_CreateObject();
        cJSON_AddItemToArray(tools, tool);
        cJSON_AddStringToObject(tool, "type", "function");
        cJSON_AddStringToObject(tool, "name", iter->name);
        cJSON_AddStringToObject(tool, "description", iter->desc);
        cJSON *parameters = cJSON_CreateObject();
        cJSON_AddItemToObject(tool, "parameters", parameters);
        cJSON_AddStringToObject(parameters, "type", "object");
        cJSON *properties = cJSON_CreateObject();
        cJSON_AddItemToObject(parameters, "properties", properties);
        int require_num = 0;
        for (int i = 0; i < iter->attr_num; i++) {
            attribute_t *attr = &iter->attr_list[i];
            cJSON *prop = cJSON_CreateObject();
            cJSON_AddItemToObject(properties, attr->name, prop);
            cJSON_AddStringToObject(prop, "type", get_attr_type(attr->type));
            cJSON_AddStringToObject(prop, "description", attr->desc);
            if (attr->type == ATTRIBUTE_TYPE_PARENT) {
                add_parent_attribute(prop, attr);
            }
            if (attr->required) {
                require_num++;
            }
        }
        if (require_num) {
            cJSON *requires = cJSON_CreateArray();
            for (int i = 0; i < iter->attr_num; i++) {
                attribute_t *attr = &iter->attr_list[i];
                if (attr->required) {
                    cJSON_AddItemToArray(requires, cJSON_CreateString(attr->name));
                }
            }
            cJSON_AddItemToObject(parameters, "required", requires);
        }
        iter = iter->next;
    }
    int ret = send_datachannel_json(root);
    cJSON_Delete(root);
    return ret;
}

/* Models are not consistent about JSON types: the same tool call may arrive with
 * "channel": 3 or "channel": "3". Accept both, and refuse anything else rather than
 * silently leaving the previous value in place. */
static bool attr_take_int(attribute_t *attr, const cJSON *value)
{
    if (cJSON_IsNumber(value)) {
        attr->i_value = value->valueint;
        return true;
    }
    if (cJSON_IsString(value) && value->valuestring) {
        char *end = NULL;
        long parsed = strtol(value->valuestring, &end, 10);
        if (end != value->valuestring) {
            attr->i_value = (int)parsed;
            return true;
        }
    }
    return false;
}

static bool attr_take_bool(attribute_t *attr, const cJSON *value)
{
    if (cJSON_IsBool(value)) {
        attr->b_state = cJSON_IsTrue(value);
        return true;
    }
    if (cJSON_IsString(value) && value->valuestring) {
        attr->b_state = (strcasecmp(value->valuestring, "true") == 0 ||
                         strcasecmp(value->valuestring, "1") == 0 ||
                         strcasecmp(value->valuestring, "on") == 0);
        return true;
    }
    return false;
}

/**
 * @return  1 when the attribute was read successfully, 0 when it is absent or unusable.
 *          Returning 1 for an unusable value is how "set channel three" once drove channel 0:
 *          the assignment never happened and the stale value from the previous call was used.
 */
static int match_and_execute(cJSON *cur, attribute_t *attr)
{
    cJSON *attr_value = cJSON_GetObjectItemCaseSensitive(cur, attr->name);
    if (!attr_value) {
        if (attr->required) {
            printf("Missing required attribute: %s\n", attr->name);
        }
        return 0;
    }
    bool ok = false;
    if (attr->type == ATTRIBUTE_TYPE_BOOL) {
        ok = attr_take_bool(attr, attr_value);
    } else if (attr->type == ATTRIBUTE_TYPE_INT) {
        ok = attr_take_int(attr, attr_value);
    } else if (attr->type == ATTRIBUTE_TYPE_PARENT && cJSON_IsObject(attr_value)) {
        for (int j = 0; j < attr->attr_num; j++) {
            match_and_execute(attr_value, &attr->attr_list[j]);
        }
        ok = true;
    }
    if (!ok) {
        char *shown = cJSON_PrintUnformatted(attr_value);
        printf("Unusable value for '%s': %s\n", attr->name, shown ? shown : "?");
        free(shown);
        return 0;
    }
    if (attr->control) {
        attr->control(attr);
    }
    return 1;
}

static int send_function_call_output(const char *call_id, const char *output_json)
{
    if (call_id == NULL || output_json == NULL) {
        return -1;
    }
    cJSON *root = cJSON_CreateObject();
    cJSON *item = cJSON_CreateObject();
    if (root == NULL || item == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(item);
        return -1;
    }
    cJSON_AddStringToObject(root, "type", "conversation.item.create");
    cJSON_AddStringToObject(item, "type", "function_call_output");
    cJSON_AddStringToObject(item, "call_id", call_id);
    cJSON_AddStringToObject(item, "output", output_json);
    cJSON_AddItemToObject(root, "item", item);
    int ret = send_datachannel_json(root);
    cJSON_Delete(root);
    if (ret != 0) {
        return ret;
    }

    /* Ask the model to continue after receiving tool output */
    cJSON *resp = cJSON_CreateObject();
    if (resp == NULL) {
        return -1;
    }
    cJSON_AddStringToObject(resp, "type", "response.create");
    ret = send_datachannel_json(resp);
    cJSON_Delete(resp);
    return ret;
}

static int execute_function_call(const char *name, const char *arguments, const char *call_id)
{
    if (name == NULL || arguments == NULL) {
        return -1;
    }
    /* GA may emit both function_call_arguments.done and response.done for one call */
    if (call_id && call_id[0] && strcmp(last_function_call_id, call_id) == 0) {
        return 0;
    }
    if (call_id && call_id[0]) {
        strncpy(last_function_call_id, call_id, sizeof(last_function_call_id) - 1);
        last_function_call_id[sizeof(last_function_call_id) - 1] = 0;
    }
    note_activity();
    printf("Function Call name=%s call_id=%s args=%s\n",
           name, call_id ? call_id : "(none)", arguments);

    cJSON *args_root = cJSON_Parse(arguments);
    if (!args_root) {
        printf("Error parsing arguments JSON\n");
        if (call_id) {
            send_function_call_output(call_id, "{\"ok\":false,\"error\":\"invalid arguments\"}");
        }
        return -1;
    }

    bool matched = false;
    int  result = -1;
    char output[192] = "{\"ok\":false,\"error\":\"unknown function\"}";
    class_t *iter = classes;
    while (iter) {
        if (strcmp(iter->name, name) == 0) {
            matched = true;
            bool missing = false;
            /* Never let a value survive from a previous call */
            for (int i = 0; i < iter->attr_num; i++) {
                iter->attr_list[i].i_value = 0;
                iter->attr_list[i].b_state = false;
            }
            for (int i = 0; i < iter->attr_num; i++) {
                attribute_t *attr = &iter->attr_list[i];
                if (!match_and_execute(args_root, attr) && attr->required) {
                    missing = true;
                }
            }
            if (missing) {
                snprintf(output, sizeof(output),
                         "{\"ok\":false,\"error\":\"an argument was missing or not a number; send integers\"}");
            } else if (iter->control) {
                result = iter->control(iter, output, sizeof(output));
            } else {
                snprintf(output, sizeof(output), "{\"ok\":true}");
                result = 0;
            }
            break;
        }
        iter = iter->next;
    }
    cJSON_Delete(args_root);
    printf("Function result: %s\n", output);

    if (call_id) {
        send_function_call_output(call_id, output);
    }
    return matched ? result : -1;
}

static int process_json(const char *json_data)
{
    cJSON *root = cJSON_Parse(json_data);
    if (!root) {
        printf("Error parsing JSON data\n");
        return -1;
    }

    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(type) || type->valuestring == NULL) {
        cJSON_Delete(root);
        return 0;
    }
    if (event_trace) {
        static int64_t t0;
        if (strcmp(type->valuestring, "response.created") == 0) {
            t0 = esp_timer_get_time();
        }
        if (strstr(type->valuestring, "audio") || strstr(type->valuestring, "response.done") ||
            strstr(type->valuestring, "response.created") || strstr(type->valuestring, "speech")) {
            printf("evt %6lld ms  %s\n", t0 ? (esp_timer_get_time() - t0) / 1000 : 0, type->valuestring);
        }
    }

    if (strcmp(type->valuestring, "error") == 0 ||
        strcmp(type->valuestring, "invalid_request_error") == 0) {
        char *payload = cJSON_PrintUnformatted(root);
        ESP_LOGE(TAG, "Realtime error: %s", payload ? payload : "(null)");
        const cJSON *err = cJSON_GetObjectItemCaseSensitive(root, "error");
        const cJSON *msg = err ? cJSON_GetObjectItemCaseSensitive(err, "message") : NULL;
        display_set_error("OpenAI error: %s", cJSON_IsString(msg) ? msg->valuestring : "see log");
        free(payload);
        cJSON_Delete(root);
        return -1;
    }

    if (strcmp(type->valuestring, "session.updated") == 0) {
        ESP_LOGI(TAG, "Realtime session.updated (tools/config applied)");
        cJSON_Delete(root);
        return 0;
    }

    if (strcmp(type->valuestring, "response.output_audio_transcript.done") == 0) {
        const cJSON *t = cJSON_GetObjectItemCaseSensitive(root, "transcript");
        if (cJSON_IsString(t) && t->valuestring[0]) {
            printf("SAID: %s\n", t->valuestring);
            display_set_transcript(t->valuestring);
        }
        cJSON_Delete(root);
        return 0;
    }

    if (strcmp(type->valuestring, "response.created") == 0 ||
        strcmp(type->valuestring, "output_audio_buffer.started") == 0) {
        mic_gate(true);
        cJSON_Delete(root);
        return 0;
    }

    /* Speaker has genuinely finished (or was cut off): listen again after a short tail. */
    if (strcmp(type->valuestring, "output_audio_buffer.stopped") == 0 ||
        strcmp(type->valuestring, "output_audio_buffer.cleared") == 0) {
        mic_gate(false);
        cJSON_Delete(root);
        return 0;
    }

    if (strcmp(type->valuestring, "conversation.item.input_audio_transcription.completed") == 0) {
        const cJSON *t = cJSON_GetObjectItemCaseSensitive(root, "transcript");
        if (cJSON_IsString(t)) {
            printf("HEARD YOU: %s\n", t->valuestring);
            note_activity();
            display_set_transcript(t->valuestring);
        }
        cJSON_Delete(root);
        return 0;
    }
    if (strcmp(type->valuestring, "input_audio_buffer.speech_started") == 0) {
        printf("VAD: speech started\n");
        cJSON_Delete(root);
        return 0;
    }
    if (strcmp(type->valuestring, "input_audio_buffer.speech_stopped") == 0) {
        printf("VAD: speech stopped\n");
        cJSON_Delete(root);
        return 0;
    }

    /* Beta/GA streaming completion event */
    if (strcmp(type->valuestring, "response.function_call_arguments.done") == 0) {
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "name");
        const cJSON *arguments = cJSON_GetObjectItemCaseSensitive(root, "arguments");
        const cJSON *call_id = cJSON_GetObjectItemCaseSensitive(root, "call_id");
        if (cJSON_IsString(name) && cJSON_IsString(arguments)) {
            execute_function_call(name->valuestring, arguments->valuestring,
                                  cJSON_IsString(call_id) ? call_id->valuestring : NULL);
        }
        cJSON_Delete(root);
        return 0;
    }

    /* GA also embeds completed function calls in response.done */
    if (strcmp(type->valuestring, "response.done") == 0) {
        const cJSON *response = cJSON_GetObjectItemCaseSensitive(root, "response");
        const cJSON *output = response ? cJSON_GetObjectItemCaseSensitive(response, "output") : NULL;
        if (cJSON_IsArray(output)) {
            cJSON *item = NULL;
            cJSON_ArrayForEach(item, output) {
                const cJSON *item_type = cJSON_GetObjectItemCaseSensitive(item, "type");
                if (!cJSON_IsString(item_type) || strcmp(item_type->valuestring, "function_call") != 0) {
                    continue;
                }
                const cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "name");
                const cJSON *arguments = cJSON_GetObjectItemCaseSensitive(item, "arguments");
                const cJSON *call_id = cJSON_GetObjectItemCaseSensitive(item, "call_id");
                if (cJSON_IsString(name) && cJSON_IsString(arguments)) {
                    execute_function_call(name->valuestring, arguments->valuestring,
                                          cJSON_IsString(call_id) ? call_id->valuestring : NULL);
                }
            }
        }
        cJSON_Delete(root);
        return 0;
    }

    cJSON_Delete(root);
    return 0;
}

static int webrtc_data_handler(esp_webrtc_custom_data_via_t via, uint8_t *data, int size, void *ctx)
{
    process_json((const char *)data);

    /* Transcripts are handled as typed events in process_json(): "SAID" for the assistant,
     * "HEARD YOU" for the microphone. The original blind search for a "transcript" field
     * matched both and printed each streaming delta, so the two speakers were indistinguishable. */
    return 0;
}

static int send_response(char *text)
{
    if (webrtc == NULL) {
        ESP_LOGE(TAG, "WebRTC not started yet");
        return -1;
    }
    cJSON *root = cJSON_CreateObject();
    cJSON *response = cJSON_CreateObject();
    cJSON *modalities = cJSON_CreateArray();
    if (root == NULL || response == NULL || modalities == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(response);
        cJSON_Delete(modalities);
        return -1;
    }
    cJSON_AddStringToObject(root, "type", "response.create");
    cJSON_AddItemToArray(modalities, cJSON_CreateString("audio"));
    cJSON_AddItemToObject(response, "output_modalities", modalities);
    cJSON_AddStringToObject(response, "instructions", text);
    cJSON_AddItemToObject(root, "response", response);
    int ret = send_datachannel_json(root);
    cJSON_Delete(root);
    return ret;
}

static int webrtc_event_handler(esp_webrtc_event_t *event, void *ctx)
{
    printf("====================Event %d======================\n", event->type);
    switch (event->type) {
        case ESP_WEBRTC_EVENT_CONNECTING:       display_set_status("Connecting to OpenAI..."); break;
        case ESP_WEBRTC_EVENT_CONNECTED:        display_set_status("Connected - opening data channel"); break;
        case ESP_WEBRTC_EVENT_CONNECT_FAILED:
            session_up = false;
            display_set_error("OpenAI connection failed");
            break;
        case ESP_WEBRTC_EVENT_DISCONNECTED:
            session_up = false;
            display_set_status("Disconnected from OpenAI");
            break;
        case ESP_WEBRTC_EVENT_DATA_CHANNEL_OPENED:
            session_up = true;
            note_activity();
            display_set_status("Listening - talk to me");
            break;
        default: break;
    }
    if (event->type == ESP_WEBRTC_EVENT_DATA_CHANNEL_CONNECTED) {
        // As ESP32 act as SCTP server, it does not create data channel automatically
        // Here manually create one data channel (OpenAI samples use label "oai-events")
        esp_peer_data_channel_cfg_t cfg = {
            .label = "oai-events",
        };
        esp_peer_handle_t peer_handle = NULL;
        esp_webrtc_get_peer_connection(webrtc, &peer_handle);
        esp_peer_create_data_channel(peer_handle, &cfg);
    }
    if (event->type == ESP_WEBRTC_EVENT_DATA_CHANNEL_OPENED) {
        /* Opening the codec reset every channel to 30 dB, so boost the mic slots now.
         * Everything except the AEC reference slot is a microphone. */
        esp_codec_dev_handle_t rec = get_record_handle();
        if (rec) {
            uint16_t mic_mask = (uint16_t)(~(1 << AEC_REF_TDM_SLOT)) & 0x0F;
            if (esp_codec_dev_set_in_channel_gain(rec, mic_mask, (float)MIC_INPUT_GAIN_DB) == 0) {
                printf("Mic gain %d dB on slots 0x%X (reference slot %d untouched)\n",
                       MIC_INPUT_GAIN_DB, mic_mask, AEC_REF_TDM_SLOT);
            }
        }
        /* Register tools first, then ask for the greeting response */
        send_function_desc();
        send_response("Greet the user briefly and say: How can I help?");
    }
    return 0;
}

void openai_set_event_trace(bool on)
{
    event_trace = on;
}

int openai_set_vad_threshold(double threshold)
{
    vad_threshold = threshold;
    if (webrtc == NULL) {
        printf("vad: stored %.2f (no active session)\n", vad_threshold);
        return 0;
    }
    printf("vad: threshold -> %.2f, re-sending session.update\n", vad_threshold);
    return send_function_desc();
}

int openai_send_text(char *text)
{
    if (webrtc == NULL) {
        ESP_LOGE(TAG, "WebRTC not started yet");
        return -1;
    }
    cJSON *root = cJSON_CreateObject();
    if (root) {
        cJSON_AddStringToObject(root, "type", "conversation.item.create");
        cJSON_AddNullToObject(root, "previous_item_id");
        cJSON *item = cJSON_CreateObject();
        if (item) {
            cJSON_AddStringToObject(item, "type", "message");
            cJSON_AddStringToObject(item, "role", "user");
        }
        cJSON *contentArray = cJSON_CreateArray();
        cJSON *contentItem = cJSON_CreateObject();
        cJSON_AddStringToObject(contentItem, "type", "input_text");
        cJSON_AddStringToObject(contentItem, "text", text);
        cJSON_AddItemToArray(contentArray, contentItem);
        cJSON_AddItemToObject(item, "content", contentArray);
        // Add the item to the root object
        cJSON_AddItemToObject(root, "item", item);
    }
    // Print the initial JSON structure
    char *send_text = cJSON_Print(root);
    if (send_text) {
        printf("Begin to send json:%s\n", send_text);
        esp_webrtc_send_custom_data(webrtc, ESP_WEBRTC_CUSTOM_DATA_VIA_DATA_CHANNEL, (uint8_t *)send_text, strlen(send_text));
        // Clean up
        free(send_text);
    }
    cJSON_Delete(root); // Free the cJSON object

    /* Creating the item only queues it; the model answers a text turn only when asked to.
     * (Audio turns get this for free from turn_detection.create_response.) */
    cJSON *resp = cJSON_CreateObject();
    if (resp) {
        cJSON_AddStringToObject(resp, "type", "response.create");
        send_datachannel_json(resp);
        cJSON_Delete(resp);
    }
    return 0;
}

int start_webrtc(void)
{
    build_classes();
    if (network_is_connected() == false) {
        ESP_LOGE(TAG, "Wifi not connected yet");
        return -1;
    }
    if (webrtc) {
        esp_webrtc_close(webrtc);
        webrtc = NULL;
    }
    esp_peer_default_cfg_t peer_cfg = {
        .agent_recv_timeout = 500,
        .ice_use_lite_mode = true,
    };
    openai_signaling_cfg_t openai_cfg = {
        .token = OPENAI_API_KEY,
        .model = model_name,
        .voice = OPENAI_DEFAULT_VOICE,
    };
    esp_webrtc_cfg_t cfg = {
        .peer_cfg = {
            /* Without this a dropped session silently reconnects forever - and keeps billing.
             * The app decides when to come back, by wake word or by `start`. */
            .no_auto_reconnect = true,
            .audio_info = {
#ifdef WEBRTC_SUPPORT_OPUS
                .codec = ESP_PEER_AUDIO_CODEC_OPUS,
                .sample_rate = 16000,
                .channel = 1,
#else
                .codec = ESP_PEER_AUDIO_CODEC_G711A,
#endif
            },
            .audio_dir = ESP_PEER_MEDIA_DIR_SEND_RECV,
            .enable_data_channel = DATA_CHANNEL_ENABLED,
            .on_custom_data = webrtc_data_handler,
            .manual_ch_create = true, // Disable esp_peer create data channel automatically
            .extra_cfg = &peer_cfg,
            .extra_size = sizeof(peer_cfg),
        },
        .signaling_cfg.extra_cfg = &openai_cfg,
        .signaling_cfg.extra_size = sizeof(openai_cfg),
        .peer_impl = esp_peer_get_default_impl(),
        .signaling_impl = esp_signaling_get_openai_signaling(),
    };
    int ret = esp_webrtc_open(&cfg, &webrtc);
    if (ret != 0) {
        ESP_LOGE(TAG, "Fail to open webrtc");
        return ret;
    }
    // Set media provider
    esp_webrtc_media_provider_t media_provider = {};
    media_sys_get_provider(&media_provider);
    esp_webrtc_set_media_provider(webrtc, &media_provider);

    // Set event handler
    esp_webrtc_set_event_handler(webrtc, webrtc_event_handler, NULL);

    // Start webrtc
    ret = esp_webrtc_start(webrtc);
    if (ret != 0) {
        ESP_LOGE(TAG, "Fail to start webrtc");
    }
    return ret;
}

void query_webrtc(void)
{
    if (webrtc) {
        esp_webrtc_query(webrtc);
    }
}

int stop_webrtc(void)
{
    session_up = false;
    mic_gate(false);
    set_mic_muted(false);
    display_set_status("Session stopped - type 'start'");
    if (webrtc) {
        esp_webrtc_handle_t handle = webrtc;
        webrtc = NULL;
        esp_webrtc_close(handle);
    }
    return 0;
}
