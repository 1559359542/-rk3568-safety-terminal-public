#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <signal.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include <MQTTClient.h>
#define JSMN_HEADER
#include "jsmn.h"

#define SNAPSHOT_PATH "/run/industrial_safety/system_state.json"
#define DATABASE_PATH "/userdata/industrial_safety/industrial_safety.db"
#define CURSOR_PATH "/userdata/industrial_safety/.mqtt_reporterd_last_event_id"
#define DEVICE_ID_DEFAULT "rk3568-001"
#define BROKER_HOST_DEFAULT "192.168.5.2"
#define BROKER_PORT_DEFAULT "1883"
#define STATUS_INTERVAL_MS 5000LL
#define RECONNECT_MAX_MS 30000LL
#define EVENT_BATCH_LIMIT 20
#define SNAPSHOT_CAPACITY 32768U
#define PAYLOAD_CAPACITY 65536U
#define TEXT_CAPACITY 512U
#define TOPIC_CAPACITY 128U
#define JSON_TOKEN_CAPACITY 512U

enum report_result {
    REPORT_MQTT_FAILED = -1,
    REPORT_OK = 0,
    REPORT_LOCAL_UNAVAILABLE = 1
};

struct json_document {
    const char *text;
    jsmntok_t tokens[JSON_TOKEN_CAPACITY];
    int count;
};

static volatile sig_atomic_t g_stop_requested;

static void request_stop(int signal_number)
{
    (void)signal_number;
    g_stop_requested = 1;
}

static long long monotonic_ms(void)
{
    struct timespec timestamp;

    if (clock_gettime(CLOCK_MONOTONIC, &timestamp) != 0)
        return 0;
    return (long long)timestamp.tv_sec * 1000LL +
           (long long)timestamp.tv_nsec / 1000000LL;
}

static void log_message(int priority, const char *message)
{
    syslog(priority, "%s", message);
    (void)fprintf(stderr, "mqtt_reporterd: %s\n", message);
    (void)fflush(stderr);
}

static const char *env_or_default(const char *name, const char *fallback)
{
    const char *value = getenv(name);

    return value != NULL && value[0] != '\0' ? value : fallback;
}

static bool format_now(char *buffer, size_t capacity)
{
    struct timespec timestamp;
    struct tm local_time;

    if (clock_gettime(CLOCK_REALTIME, &timestamp) != 0 ||
        localtime_r(&timestamp.tv_sec, &local_time) == NULL)
        return false;
    return strftime(buffer, capacity, "%Y-%m-%dT%H:%M:%S%z", &local_time) != 0U;
}

static int read_text_file(const char *path, char *buffer, size_t capacity)
{
    FILE *file = fopen(path, "r");
    size_t length;
    int read_failed;

    if (file == NULL)
        return -1;
    length = fread(buffer, 1U, capacity - 1U, file);
    read_failed = ferror(file);
    if (fclose(file) != 0 || read_failed != 0)
        return -1;
    buffer[length] = '\0';
    return length > 0U ? 0 : -1;
}

static int json_parse(struct json_document *document, const char *text)
{
    jsmn_parser parser;

    document->text = text;
    jsmn_init(&parser);
    document->count = jsmn_parse(&parser, text, strlen(text), document->tokens,
                                 JSON_TOKEN_CAPACITY);
    return document->count > 0 && document->tokens[0].type == JSMN_OBJECT ? 0 : -1;
}

static bool token_equals(const struct json_document *document, int token_index,
                         const char *text)
{
    const jsmntok_t *token = &document->tokens[token_index];
    size_t length = (size_t)(token->end - token->start);

    return token->type == JSMN_STRING && strlen(text) == length &&
           strncmp(document->text + token->start, text, length) == 0;
}

static int token_next(const struct json_document *document, int token_index)
{
    int end = document->tokens[token_index].end;
    int next = token_index + 1;

    while (next < document->count && document->tokens[next].start < end)
        next++;
    return next;
}

static int json_object_get(const struct json_document *document, int object_index,
                           const char *key)
{
    const jsmntok_t *object;
    int index;

    if (object_index < 0 || object_index >= document->count)
        return -1;
    object = &document->tokens[object_index];
    if (object->type != JSMN_OBJECT)
        return -1;
    index = object_index + 1;
    while (index + 1 < document->count &&
           document->tokens[index].start < object->end) {
        int value_index = index + 1;

        if (token_equals(document, index, key))
            return value_index;
        index = token_next(document, value_index);
    }
    return -1;
}

static int json_path_get(const struct json_document *document,
                         const char *object_key, const char *value_key)
{
    int object_index = json_object_get(document, 0, object_key);

    return json_object_get(document, object_index, value_key);
}

static int token_copy(const struct json_document *document, int token_index,
                      char *buffer, size_t capacity)
{
    const jsmntok_t *token;
    size_t length;

    if (token_index < 0 || token_index >= document->count)
        return -1;
    token = &document->tokens[token_index];
    if (token->type != JSMN_STRING)
        return -1;
    length = (size_t)(token->end - token->start);
    if (length + 1U > capacity)
        return -1;
    memcpy(buffer, document->text + token->start, length);
    buffer[length] = '\0';
    return 0;
}

static int token_number(const struct json_document *document, int token_index,
                        double *value)
{
    const jsmntok_t *token;
    char text[64];
    char *end;
    size_t length;

    if (token_index < 0 || token_index >= document->count)
        return -1;
    token = &document->tokens[token_index];
    if (token->type != JSMN_PRIMITIVE)
        return -1;
    length = (size_t)(token->end - token->start);
    if (length == 0U || length + 1U > sizeof(text))
        return -1;
    memcpy(text, document->text + token->start, length);
    text[length] = '\0';
    if (strcmp(text, "null") == 0)
        return -1;
    errno = 0;
    *value = strtod(text, &end);
    return errno == 0 && *end == '\0' ? 0 : -1;
}

static int token_bool(const struct json_document *document, int token_index,
                      bool *value)
{
    const jsmntok_t *token;
    size_t length;
    const char *text;

    if (token_index < 0 || token_index >= document->count)
        return -1;
    token = &document->tokens[token_index];
    if (token->type != JSMN_PRIMITIVE)
        return -1;
    text = document->text + token->start;
    length = (size_t)(token->end - token->start);
    if (length == 4U && strncmp(text, "true", 4U) == 0) {
        *value = true;
        return 0;
    }
    if (length == 5U && strncmp(text, "false", 5U) == 0) {
        *value = false;
        return 0;
    }
    return -1;
}

static void json_escape(const char *source, char *target, size_t capacity)
{
    size_t used = 0U;

    while (*source != '\0' && used + 2U < capacity) {
        unsigned char character = (unsigned char)*source++;

        if (character == '\"' || character == '\\') {
            target[used++] = '\\';
            target[used++] = (char)character;
        } else if (character == '\n') {
            target[used++] = '\\'; target[used++] = 'n';
        } else if (character == '\r') {
            target[used++] = '\\'; target[used++] = 'r';
        } else if (character == '\t') {
            target[used++] = '\\'; target[used++] = 't';
        } else if (character >= 0x20U) {
            target[used++] = (char)character;
        }
    }
    target[used] = '\0';
}

static const char *state_zh(const char *state)
{
    if (strcmp(state, "online") == 0) return "在线";
    if (strcmp(state, "running") == 0) return "运行中";
    if (strcmp(state, "starting") == 0) return "启动中";
    if (strcmp(state, "streaming") == 0) return "推流中";
    if (strcmp(state, "active") == 0) return "告警激活";
    if (strcmp(state, "inactive") == 0 || strcmp(state, "normal") == 0) return "正常";
    if (strcmp(state, "offline") == 0) return "离线";
    if (strcmp(state, "closed") == 0) return "门已关闭";
    if (strcmp(state, "open") == 0) return "门已打开";
    if (strcmp(state, "unavailable") == 0) return "不可用";
    return "未知";
}

static bool state_is_healthy(const char *state)
{
    return strcmp(state, "online") == 0 || strcmp(state, "running") == 0 ||
           strcmp(state, "starting") == 0;
}

static const char *door_state_zh(const char *state)
{
    if (strcmp(state, "active") == 0 || strcmp(state, "open") == 0)
        return "门已打开";
    if (strcmp(state, "inactive") == 0 || strcmp(state, "closed") == 0)
        return "门已关闭";
    return "状态未知";
}

static void format_nullable_number(bool valid, double value,
                                   char *buffer, size_t capacity)
{
    if (valid)
        (void)snprintf(buffer, capacity, "%.3f", value);
    else
        (void)snprintf(buffer, capacity, "null");
}

static const char *source_zh(const char *source)
{
    if (strcmp(source, "intrusion") == 0) return "人员入侵";
    if (strcmp(source, "door") == 0) return "门磁";
    if (strcmp(source, "ai") == 0) return "AI 人员检测";
    if (strcmp(source, "radar") == 0) return "雷达";
    if (strcmp(source, "temperature") == 0) return "温度传感器";
    if (strcmp(source, "humidity") == 0) return "湿度传感器";
    if (strcmp(source, "light") == 0) return "光照传感器";
    if (strcmp(source, "environment") == 0) return "环境监测";
    if (strcmp(source, "camera") == 0) return "摄像头";
    if (strcmp(source, "daemon") == 0) return "告警服务";
    if (strcmp(source, "sht3x") == 0) return "温湿度传感器";
    if (strcmp(source, "bh1750") == 0) return "光照传感器";
    if (strcmp(source, "system") == 0) return "系统";
    return "未知来源";
}

static const char *event_zh(const char *source, const char *event_type)
{
    if (strcmp(source, "intrusion") == 0 && strcmp(event_type, "enter") == 0) return "人员进入危险区域";
    if (strcmp(source, "intrusion") == 0 && strcmp(event_type, "clear") == 0) return "人员入侵告警已解除";
    if (strcmp(source, "door") == 0 && strcmp(event_type, "open") == 0) return "门已打开";
    if (strcmp(source, "door") == 0 && strcmp(event_type, "close") == 0) return "门已关闭";
    if (strcmp(event_type, "overheat") == 0) return "温度过高";
    if (strcmp(event_type, "overexposure") == 0) return "光照过强";
    if (strcmp(event_type, "suspected_fire") == 0) return "疑似火灾或设备异常发热";
    if (strcmp(event_type, "suspected_fire_clear") == 0) return "疑似火灾告警已解除";
    if (strcmp(source, "temperature") == 0 && strcmp(event_type, "recovery") == 0) return "温度恢复正常";
    if (strcmp(source, "light") == 0 && strcmp(event_type, "recovery") == 0) return "光照恢复正常";
    if (strcmp(source, "sht3x") == 0 && strcmp(event_type, "humidity_high") == 0) return "湿度过高";
    if (strcmp(source, "bh1750") == 0 && strcmp(event_type, "lux_low") == 0) return "光照过低";
    if (strcmp(event_type, "offline") == 0) return "模块离线";
    if (strcmp(event_type, "recovery") == 0) return "模块恢复";
    if (strcmp(event_type, "event") == 0) return "告警触发";
    if (strcmp(event_type, "clear") == 0) return "告警解除";
    if (strcmp(event_type, "startup") == 0) return "服务启动";
    if (strcmp(event_type, "shutdown") == 0) return "服务停止";
    if (strcmp(event_type, "snapshot_invalid") == 0) return "状态快照异常";
    if (strcmp(event_type, "snapshot_recovery") == 0) return "状态快照恢复";
    if (strcmp(event_type, "abnormal_exit") == 0) return "服务曾异常退出";
    return "状态变化";
}

static const char *level_zh(const char *level)
{
    if (strcmp(level, "alarm") == 0) return "告警";
    if (strcmp(level, "warning") == 0) return "预警";
    if (strcmp(level, "info") == 0) return "提示";
    return "未知等级";
}

static const char *unit_for_event(const char *source, const char *event_type)
{
    if (strcmp(source, "temperature") == 0) return "摄氏度";
    if (strcmp(source, "humidity") == 0 ||
        (strcmp(source, "sht3x") == 0 &&
         strcmp(event_type, "humidity_high") == 0)) return "%RH";
    if (strcmp(source, "light") == 0) return "勒克斯";
    if (strcmp(source, "bh1750") == 0) return "勒克斯";
    if (strcmp(source, "radar") == 0) return "毫米";
    return "";
}

static bool load_cursor(long long *cursor)
{
    char buffer[64];
    char *end;
    long long value;

    if (read_text_file(CURSOR_PATH, buffer, sizeof(buffer)) != 0)
        return false;
    errno = 0;
    value = strtoll(buffer, &end, 10);
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')
        end++;
    if (errno != 0 || end == buffer || *end != '\0' || value < 0)
        return false;
    *cursor = value;
    return true;
}

static int save_cursor(long long event_id)
{
    char temporary[160];
    FILE *file;
    int failed = 0;

    if (snprintf(temporary, sizeof(temporary), "%s.tmp", CURSOR_PATH) >= (int)sizeof(temporary))
        return -1;
    file = fopen(temporary, "w");
    if (file == NULL)
        return -1;
    if (fprintf(file, "%lld\n", event_id) < 0 || fflush(file) != 0 ||
        fsync(fileno(file)) != 0)
        failed = 1;
    if (fclose(file) != 0)
        failed = 1;
    if (failed != 0) {
        (void)unlink(temporary);
        return -1;
    }
    if (rename(temporary, CURSOR_PATH) != 0) {
        (void)unlink(temporary);
        return -1;
    }
    return 0;
}

static int publish(MQTTClient client, const char *topic, const char *payload, int retained)
{
    MQTTClient_message message = MQTTClient_message_initializer;
    MQTTClient_deliveryToken token = 0;
    int rc;

    message.payload = (void *)payload;
    message.payloadlen = (int)strlen(payload);
    message.qos = 1;
    message.retained = retained;
    rc = MQTTClient_publishMessage(client, topic, &message, &token);
    return rc == MQTTCLIENT_SUCCESS ? MQTTClient_waitForCompletion(client, token, 5000L) : rc;
}

static enum report_result publish_status(MQTTClient client, const char *topic,
                                         const char *device_id)
{
    struct json_document document;
    char snapshot[SNAPSHOT_CAPACITY];
    char camera[64] = "unknown", ai[64] = "unknown", preview[64] = "unavailable";
    char alarm_state[64] = "unknown", door_state[64] = "unknown", radar_state[64] = "unknown";
    char sht3x_state[64] = "unknown", bh1750_state[64] = "unknown";
    char now[40] = "未知时间", device_escaped[TEXT_CAPACITY], payload[PAYLOAD_CAPACITY];
    char camera_escaped[TEXT_CAPACITY], ai_escaped[TEXT_CAPACITY];
    char preview_escaped[TEXT_CAPACITY], alarm_escaped[TEXT_CAPACITY];
    char door_escaped[TEXT_CAPACITY], radar_escaped[TEXT_CAPACITY];
    char sht3x_escaped[TEXT_CAPACITY], bh1750_escaped[TEXT_CAPACITY];
    char temperature_text[64], humidity_text[64], lux_text[64];
    char fps_text[64], latency_text[64];
    bool ai_alarm = false, radar_alarm = false, roi_active = false;
    bool have_ai_alarm, have_radar_alarm, have_roi_active;
    bool intrusion_known, intrusion_active;
    bool have_temperature, have_humidity, have_lux, have_fps, have_latency;
    double temperature = 0.0, humidity = 0.0, lux = 0.0, fps = 0.0, latency = 0.0;
    const char *intrusion_json;
    const char *intrusion_zh;
    const char *roi_json;
    const char *ai_alarm_json;
    const char *radar_alarm_json;
    int rc;
    int length;

    if (read_text_file(SNAPSHOT_PATH, snapshot, sizeof(snapshot)) != 0 ||
        json_parse(&document, snapshot) != 0) {
        log_message(LOG_WARNING, "状态快照不可读，本轮不发布 status");
        return REPORT_LOCAL_UNAVAILABLE;
    }
    (void)token_copy(&document, json_object_get(&document, 0, "camera_state"),
                     camera, sizeof(camera));
    (void)token_copy(&document, json_object_get(&document, 0, "ai_state"),
                     ai, sizeof(ai));
    (void)token_copy(&document,
                     json_object_get(&document, 0, "preview_stream_state"),
                     preview, sizeof(preview));
    (void)token_copy(&document, json_path_get(&document, "alarm", "state"),
                     alarm_state, sizeof(alarm_state));
    (void)token_copy(&document, json_path_get(&document, "door", "logical_state"),
                     door_state, sizeof(door_state));
    (void)token_copy(&document, json_path_get(&document, "ld2410", "state"),
                     radar_state, sizeof(radar_state));
    (void)token_copy(&document, json_path_get(&document, "sht3x", "state"),
                     sht3x_state, sizeof(sht3x_state));
    (void)token_copy(&document, json_path_get(&document, "bh1750", "state"),
                     bh1750_state, sizeof(bh1750_state));
    have_ai_alarm = token_bool(&document,
                               json_path_get(&document, "alarm", "ai_alarm"),
                               &ai_alarm) == 0;
    have_radar_alarm = token_bool(&document,
                                  json_path_get(&document, "alarm", "radar_alarm"),
                                  &radar_alarm) == 0;
    have_roi_active = token_bool(&document,
                                 json_object_get(&document, 0, "roi_active"),
                                 &roi_active) == 0;
    have_temperature = token_number(&document,
                                     json_path_get(&document, "sht3x", "temperature_c"),
                                     &temperature) == 0;
    have_humidity = token_number(&document,
                                  json_path_get(&document, "sht3x", "humidity_rh"),
                                  &humidity) == 0;
    have_lux = token_number(&document,
                            json_path_get(&document, "bh1750", "value_lux"),
                            &lux) == 0;
    have_fps = token_number(&document,
                            json_object_get(&document, 0, "preview_fps"),
                            &fps) == 0;
    have_latency = token_number(&document,
                                json_object_get(&document, 0, "preview_latency_ms"),
                                &latency) == 0;
    have_temperature = have_temperature && state_is_healthy(sht3x_state);
    have_humidity = have_humidity && state_is_healthy(sht3x_state);
    have_lux = have_lux && state_is_healthy(bh1750_state);
    have_fps = have_fps && state_is_healthy(ai) &&
               strcmp(preview, "unavailable") != 0;
    have_latency = have_latency && state_is_healthy(ai) &&
                   strcmp(preview, "unavailable") != 0;

    intrusion_active = (have_ai_alarm && ai_alarm) ||
                       (have_radar_alarm && radar_alarm);
    intrusion_known = intrusion_active ||
                      (state_is_healthy(ai) && state_is_healthy(radar_state) &&
                       have_ai_alarm && have_radar_alarm);
    intrusion_json = intrusion_known ? (intrusion_active ? "true" : "false") : "null";
    intrusion_zh = intrusion_known ?
                   (intrusion_active ? "检测到人员闯入" : "未检测到人员闯入") :
                   "状态未知";
    roi_json = have_roi_active ? (roi_active ? "true" : "false") : "null";
    ai_alarm_json = have_ai_alarm ? (ai_alarm ? "true" : "false") : "null";
    radar_alarm_json = have_radar_alarm ?
                       (radar_alarm ? "true" : "false") : "null";
    format_nullable_number(have_temperature, temperature,
                           temperature_text, sizeof(temperature_text));
    format_nullable_number(have_humidity, humidity,
                           humidity_text, sizeof(humidity_text));
    format_nullable_number(have_lux, lux, lux_text, sizeof(lux_text));
    format_nullable_number(have_fps, fps, fps_text, sizeof(fps_text));
    format_nullable_number(have_latency, latency,
                           latency_text, sizeof(latency_text));
    (void)format_now(now, sizeof(now));
    json_escape(device_id, device_escaped, sizeof(device_escaped));
    json_escape(camera, camera_escaped, sizeof(camera_escaped));
    json_escape(ai, ai_escaped, sizeof(ai_escaped));
    json_escape(preview, preview_escaped, sizeof(preview_escaped));
    json_escape(alarm_state, alarm_escaped, sizeof(alarm_escaped));
    json_escape(door_state, door_escaped, sizeof(door_escaped));
    json_escape(radar_state, radar_escaped, sizeof(radar_escaped));
    json_escape(sht3x_state, sht3x_escaped, sizeof(sht3x_escaped));
    json_escape(bh1750_state, bh1750_escaped, sizeof(bh1750_escaped));
    length = snprintf(payload, sizeof(payload),
        "{\"设备ID\":\"%s\",\"上报时间\":\"%s\",\"消息类型\":\"实时状态\","
        "\"状态摘要\":{\"摄像头\":\"%s\",\"摄像头中文\":\"%s\","
        "\"AI推理\":\"%s\",\"AI推理中文\":\"%s\","
        "\"告警状态\":\"%s\",\"告警状态中文\":\"%s\","
        "\"人员入侵\":%s,\"人员入侵中文\":\"%s\","
        "\"AI告警\":%s,\"雷达告警\":%s,\"AI危险区检测\":%s,"
        "\"门磁状态\":\"%s\",\"门磁状态中文\":\"%s\","
        "\"雷达状态\":\"%s\",\"雷达状态中文\":\"%s\","
        "\"温湿度传感器状态\":\"%s\",\"温湿度传感器状态中文\":\"%s\","
        "\"光照传感器状态\":\"%s\",\"光照传感器状态中文\":\"%s\","
        "\"温度摄氏度\":%s,\"湿度百分比\":%s,\"光照勒克斯\":%s,"
        "\"预览状态\":\"%s\",\"预览帧率\":%s,\"预览延迟毫秒\":%s},"
        "\"原始状态\":%s}",
        device_escaped, now, camera_escaped, state_zh(camera),
        ai_escaped, state_zh(ai), alarm_escaped, state_zh(alarm_state),
        intrusion_json, intrusion_zh, ai_alarm_json, radar_alarm_json, roi_json,
        door_escaped, door_state_zh(door_state),
        radar_escaped, state_zh(radar_state),
        sht3x_escaped, state_zh(sht3x_state),
        bh1750_escaped, state_zh(bh1750_state),
        temperature_text, humidity_text, lux_text, preview_escaped, fps_text,
        latency_text, snapshot);
    if (length < 0 || (size_t)length >= sizeof(payload)) {
        log_message(LOG_ERR, "status 消息过长，拒绝发布");
        return REPORT_LOCAL_UNAVAILABLE;
    }
    rc = publish(client, topic, payload, 1);
    return rc == MQTTCLIENT_SUCCESS ? REPORT_OK : REPORT_MQTT_FAILED;
}

static int initialize_cursor(sqlite3 *database, long long *cursor)
{
    static const char sql[] = "SELECT COALESCE(MAX(event_id), 0) FROM events;";
    sqlite3_stmt *statement = NULL;
    int rc;

    rc = sqlite3_prepare_v2(database, sql, -1, &statement, NULL);
    if (rc != SQLITE_OK)
        return -1;
    rc = sqlite3_step(statement);
    if (rc == SQLITE_ROW) {
        *cursor = sqlite3_column_int64(statement, 0);
        rc = SQLITE_DONE;
    }
    sqlite3_finalize(statement);
    if (rc != SQLITE_DONE)
        return -1;
    if (save_cursor(*cursor) != 0)
        log_message(LOG_WARNING, "初始 event_id 游标保存失败，本次运行仍从当前事件之后上报");
    return 0;
}

static enum report_result publish_events(MQTTClient client, const char *topic,
                                         const char *device_id,
                                         long long *cursor,
                                         bool *cursor_ready)
{
    static const char sql[] =
        "SELECT event_id,event_datetime,source,event_type,level,active,value,threshold,detail,evidence_path "
        "FROM events WHERE event_id > ? ORDER BY event_id ASC LIMIT 20;";
    sqlite3 *database = NULL;
    sqlite3_stmt *statement = NULL;
    enum report_result result = REPORT_LOCAL_UNAVAILABLE;
    int step_result;

    if (sqlite3_open_v2(DATABASE_PATH, &database, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        log_message(LOG_WARNING, "SQLite 事件数据库不可读，本轮不发布 event");
        goto out;
    }
    sqlite3_busy_timeout(database, 1000);
    if (!*cursor_ready) {
        if (initialize_cursor(database, cursor) != 0) {
            log_message(LOG_ERR, "无法取得初始 event_id，本轮不发布 event");
            goto out;
        }
        *cursor_ready = true;
        log_message(LOG_INFO, "首次启动游标已定位到当前最新事件，只上报后续新事件");
    }
    if (sqlite3_prepare_v2(database, sql, -1, &statement, NULL) != SQLITE_OK) {
        log_message(LOG_ERR, "SQLite 事件查询准备失败");
        goto out;
    }
    (void)sqlite3_bind_int64(statement, 1, (sqlite3_int64)*cursor);
    while ((step_result = sqlite3_step(statement)) == SQLITE_ROW) {
        long long event_id = sqlite3_column_int64(statement, 0);
        const char *event_time = (const char *)sqlite3_column_text(statement, 1);
        const char *source = (const char *)sqlite3_column_text(statement, 2);
        const char *event_type = (const char *)sqlite3_column_text(statement, 3);
        const char *level = (const char *)sqlite3_column_text(statement, 4);
        const char *detail = (const char *)sqlite3_column_text(statement, 8);
        const char *evidence = (const char *)sqlite3_column_text(statement, 9);
        char device_escaped[TEXT_CAPACITY], time_escaped[TEXT_CAPACITY];
        char source_escaped[TEXT_CAPACITY], type_escaped[TEXT_CAPACITY];
        char level_escaped[TEXT_CAPACITY], detail_escaped[TEXT_CAPACITY];
        char evidence_escaped[TEXT_CAPACITY], value_text[64];
        char threshold_text[64], value_zh[96], threshold_zh[96];
        char summary[1024], summary_escaped[2048], payload[4096];
        const char *unit;
        bool has_value = sqlite3_column_type(statement, 6) != SQLITE_NULL;
        bool has_threshold = sqlite3_column_type(statement, 7) != SQLITE_NULL;
        int active = sqlite3_column_int(statement, 5);
        int length;

        if (source == NULL || event_type == NULL || level == NULL) {
            log_message(LOG_ERR, "事件缺少 source、event_type 或 level，跳过损坏记录");
            *cursor = event_id;
            if (save_cursor(*cursor) != 0)
                log_message(LOG_WARNING, "损坏事件后的 event_id 游标保存失败");
            continue;
        }
        if (has_value)
            (void)snprintf(value_text, sizeof(value_text), "%.3f", sqlite3_column_double(statement, 6));
        else
            (void)snprintf(value_text, sizeof(value_text), "null");
        if (has_threshold)
            (void)snprintf(threshold_text, sizeof(threshold_text), "%.3f", sqlite3_column_double(statement, 7));
        else
            (void)snprintf(threshold_text, sizeof(threshold_text), "null");
        json_escape(device_id, device_escaped, sizeof(device_escaped));
        json_escape(event_time == NULL ? "未知时间" : event_time,
                    time_escaped, sizeof(time_escaped));
        json_escape(source, source_escaped, sizeof(source_escaped));
        json_escape(event_type, type_escaped, sizeof(type_escaped));
        json_escape(level, level_escaped, sizeof(level_escaped));
        json_escape(detail == NULL ? "" : detail, detail_escaped, sizeof(detail_escaped));
        json_escape(evidence == NULL ? "" : evidence, evidence_escaped, sizeof(evidence_escaped));
        unit = unit_for_event(source, event_type);
        if (has_value)
            (void)snprintf(value_zh, sizeof(value_zh), "%.3f %s",
                           sqlite3_column_double(statement, 6), unit);
        else
            (void)snprintf(value_zh, sizeof(value_zh), "未提供");
        if (has_threshold)
            (void)snprintf(threshold_zh, sizeof(threshold_zh), "%.3f %s",
                           sqlite3_column_double(statement, 7), unit);
        else
            (void)snprintf(threshold_zh, sizeof(threshold_zh), "未提供");
        (void)snprintf(summary, sizeof(summary),
                       "%s，%s发生“%s”，等级：%s，状态：%s，数值：%s，阈值：%s。",
                       event_time == NULL ? "未知时间" : event_time,
                       source_zh(source), event_zh(source, event_type), level_zh(level),
                       active ? "已激活" : "已解除或未激活", value_zh, threshold_zh);
        json_escape(summary, summary_escaped, sizeof(summary_escaped));
        length = snprintf(payload, sizeof(payload),
            "{\"设备ID\":\"%s\",\"事件ID\":%lld,\"事件时间\":\"%s\","
            "\"消息类型\":\"安全事件\",\"中文摘要\":\"%s\","
            "\"事件来源\":\"%s\",\"事件来源中文\":\"%s\","
            "\"事件类型\":\"%s\",\"事件类型中文\":\"%s\","
            "\"告警等级\":\"%s\",\"告警等级中文\":\"%s\","
            "\"是否激活\":%s,\"事件说明\":\"%s\","
            "\"数值\":%s,\"数值单位\":\"%s\",\"阈值\":%s,"
            "\"阈值单位\":\"%s\",\"证据路径\":\"%s\"}",
            device_escaped, event_id, time_escaped, summary_escaped,
            source_escaped, source_zh(source), type_escaped, event_zh(source, event_type),
            level_escaped, level_zh(level), active ? "true" : "false", detail_escaped,
            value_text, unit, threshold_text, unit, evidence_escaped);
        if (length < 0 || (size_t)length >= sizeof(payload)) {
            log_message(LOG_ERR, "event 消息过长，本轮不发布该事件");
            goto out;
        }
        if (publish(client, topic, payload, 0) != MQTTCLIENT_SUCCESS) {
            log_message(LOG_WARNING, "event 发布失败，将在重连后重试");
            result = REPORT_MQTT_FAILED;
            goto out;
        }
        *cursor = event_id;
        if (save_cursor(*cursor) != 0)
            log_message(LOG_WARNING, "event_id 游标保存失败，重启后可能重复上报");
    }
    if (step_result != SQLITE_DONE) {
        log_message(LOG_ERR, "SQLite 事件查询执行失败，本轮停止读取 event");
        goto out;
    }
    result = REPORT_OK;
out:
    if (statement != NULL) sqlite3_finalize(statement);
    if (database != NULL) sqlite3_close(database);
    return result;
}

int main(void)
{
    char address[TOPIC_CAPACITY], client_id[TOPIC_CAPACITY], availability_topic[TOPIC_CAPACITY];
    char status_topic[TOPIC_CAPACITY], event_topic[TOPIC_CAPACITY];
    const char *device_id = env_or_default("MQTT_DEVICE_ID", DEVICE_ID_DEFAULT);
    const char *host = env_or_default("MQTT_BROKER_HOST", BROKER_HOST_DEFAULT);
    const char *port = env_or_default("MQTT_BROKER_PORT", BROKER_PORT_DEFAULT);
    MQTTClient client = NULL;
    const char *replay_history = env_or_default("MQTT_REPLAY_HISTORY", "0");
    long long cursor = 0, next_status = 0, next_connect = 0, retry_ms = 1000;
    bool cursor_ready;
    bool connected = false;
    struct sigaction action;
    int text_length;

    openlog("mqtt_reporterd", LOG_PID, LOG_DAEMON);
    cursor_ready = load_cursor(&cursor);
    if (!cursor_ready && strcmp(replay_history, "1") == 0) {
        cursor = 0;
        cursor_ready = true;
        log_message(LOG_INFO, "未找到有效游标，已按配置从历史第一条事件开始补传");
    } else if (!cursor_ready) {
        log_message(LOG_INFO, "未找到有效游标，将从数据库当前最新事件之后开始上报");
    }
    memset(&action, 0, sizeof(action));
    action.sa_handler = request_stop;
    sigemptyset(&action.sa_mask);
    (void)sigaction(SIGINT, &action, NULL);
    (void)sigaction(SIGTERM, &action, NULL);
    text_length = snprintf(address, sizeof(address), "tcp://%s:%s", host, port);
    if (text_length < 0 || (size_t)text_length >= sizeof(address))
        goto invalid_configuration;
    text_length = snprintf(client_id, sizeof(client_id), "%s-mqtt-reporterd", device_id);
    if (text_length < 0 || (size_t)text_length >= sizeof(client_id))
        goto invalid_configuration;
    text_length = snprintf(availability_topic, sizeof(availability_topic),
                           "device/%s/availability", device_id);
    if (text_length < 0 || (size_t)text_length >= sizeof(availability_topic))
        goto invalid_configuration;
    text_length = snprintf(status_topic, sizeof(status_topic),
                           "device/%s/status", device_id);
    if (text_length < 0 || (size_t)text_length >= sizeof(status_topic))
        goto invalid_configuration;
    text_length = snprintf(event_topic, sizeof(event_topic),
                           "device/%s/event", device_id);
    if (text_length < 0 || (size_t)text_length >= sizeof(event_topic))
        goto invalid_configuration;
    if (MQTTClient_create(&client, address, client_id, MQTTCLIENT_PERSISTENCE_NONE, NULL) != MQTTCLIENT_SUCCESS) {
        log_message(LOG_ERR, "MQTTClient_create 失败");
        return 1;
    }
    while (!g_stop_requested) {
        long long now = monotonic_ms();

        if (!connected && now >= next_connect) {
            MQTTClient_connectOptions options = MQTTClient_connectOptions_initializer;
            MQTTClient_willOptions will = MQTTClient_willOptions_initializer;
            int rc;

            will.topicName = availability_topic;
            will.message = "offline";
            will.qos = 1;
            will.retained = 1;
            options.keepAliveInterval = 20;
            options.cleansession = 1;
            options.connectTimeout = 5;
            options.will = &will;
            rc = MQTTClient_connect(client, &options);
            if (rc == MQTTCLIENT_SUCCESS) {
                if (publish(client, availability_topic, "online", 1) ==
                    MQTTCLIENT_SUCCESS) {
                    connected = true;
                    retry_ms = 1000;
                    next_status = 0;
                    log_message(LOG_INFO, "MQTT 已连接，开始只读上报");
                } else {
                    MQTTClient_disconnect(client, 1000);
                    next_connect = now + retry_ms;
                    retry_ms = retry_ms < RECONNECT_MAX_MS / 2 ?
                               retry_ms * 2 : RECONNECT_MAX_MS;
                    log_message(LOG_WARNING, "MQTT online 状态发布失败，等待后重试");
                }
            } else {
                next_connect = now + retry_ms;
                retry_ms = retry_ms < RECONNECT_MAX_MS / 2 ? retry_ms * 2 : RECONNECT_MAX_MS;
                log_message(LOG_WARNING, "MQTT 连接失败，等待后重试");
            }
        }
        if (connected) {
            if (now >= next_status) {
                enum report_result status_result =
                    publish_status(client, status_topic, device_id);

                if (status_result == REPORT_MQTT_FAILED) {
                    MQTTClient_disconnect(client, 1000);
                    connected = false;
                    next_connect = now + retry_ms;
                    continue;
                }
                next_status = now + STATUS_INTERVAL_MS;
            }
            if (publish_events(client, event_topic, device_id, &cursor,
                               &cursor_ready) == REPORT_MQTT_FAILED) {
                MQTTClient_disconnect(client, 1000);
                connected = false;
                next_connect = now + retry_ms;
            }
        }
        sleep(1);
    }
    if (connected) {
        (void)publish(client, availability_topic, "offline", 1);
        MQTTClient_disconnect(client, 3000);
    }
    MQTTClient_destroy(&client);
    closelog();
    return 0;

invalid_configuration:
    log_message(LOG_ERR, "MQTT 配置过长，无法生成 broker 地址或主题");
    closelog();
    return 1;
}
