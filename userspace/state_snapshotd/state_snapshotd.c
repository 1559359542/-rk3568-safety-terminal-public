#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define AI_STATE_PATH "/opt/rk3568_yolov5_demo/hmi_runtime/ai_runtime.json"
#define DAEMON_SOCKET "/run/safety_alarmd.sock"
#define OUTPUT_DIR "/run/industrial_safety"
#define OUTPUT_PATH OUTPUT_DIR "/system_state.json"
#define TEMP_PATH OUTPUT_DIR "/.system_state.tmp.json"
#define BUFFER_SIZE 16384U
#define AI_HEALTH_TIMEOUT_MS 5000ULL

static volatile sig_atomic_t stop_requested;

static void request_stop(int signum)
{
    (void)signum;
    stop_requested = 1;
}

static int read_file(const char *path, char *buffer, size_t capacity)
{
    FILE *file = fopen(path, "r");
    size_t length;

    if (file == NULL)
        return -1;
    length = fread(buffer, 1U, capacity - 1U, file);
    if (ferror(file) != 0 || fclose(file) != 0 || length == 0U)
        return -1;
    buffer[length] = '\0';
    return 0;
}

static int request_snapshot(char *buffer, size_t capacity)
{
    struct sockaddr_un address;
    ssize_t bytes;
    int fd;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", DAEMON_SOCKET);
    if (connect(fd, (const struct sockaddr *)&address, sizeof(address)) < 0 ||
        write(fd, "snapshot\n", 9U) != 9) {
        close(fd);
        return -1;
    }
    bytes = read(fd, buffer, capacity - 1U);
    close(fd);
    if (bytes <= 0)
        return -1;
    buffer[bytes] = '\0';
    return strncmp(buffer, "error ", 6U) == 0 ? -1 : 0;
}

static int json_number(const char *json, const char *key, char *value, size_t size)
{
    char needle[96];
    const char *start;
    size_t length = 0U;

    snprintf(needle, sizeof(needle), "\"%s\":", key);
    start = strstr(json, needle);
    if (start == NULL)
        return -1;
    start += strlen(needle);
    while (start[length] != '\0' && start[length] != ',' &&
           start[length] != '}' && start[length] != '\n' && length + 1U < size)
        ++length;
    memcpy(value, start, length);
    value[length] = '\0';
    return 0;
}

static int json_string(const char *json, const char *key, char *value, size_t size)
{
    char needle[96];
    const char *start;
    const char *end;
    size_t length;

    snprintf(needle, sizeof(needle), "\"%s\":\"", key);
    start = strstr(json, needle);
    if (start == NULL)
        return -1;
    start += strlen(needle);
    end = strchr(start, '\"');
    if (end == NULL)
        return -1;
    length = (size_t)(end - start);
    if (length + 1U > size)
        return -1;
    memcpy(value, start, length);
    value[length] = '\0';
    return 0;
}

static int json_object(const char *json, const char *key, char *value, size_t size)
{
    char needle[96];
    const char *start;
    const char *end;
    int depth = 0;
    size_t length;

    snprintf(needle, sizeof(needle), "\"%s\":", key);
    start = strstr(json, needle);
    if (start == NULL)
        return -1;
    start += strlen(needle);
    if (*start != '{')
        return -1;
    end = start;
    do {
        if (*end == '{')
            ++depth;
        else if (*end == '}')
            --depth;
        ++end;
    } while (*end != '\0' && depth > 0);
    if (depth != 0)
        return -1;
    length = (size_t)(end - start);
    if (length + 1U > size)
        return -1;
    memcpy(value, start, length);
    value[length] = '\0';
    return 0;
}

static int monotonic_ms(unsigned long long *value)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return -1;
    *value = (unsigned long long)now.tv_sec * 1000ULL +
             (unsigned long long)(now.tv_nsec / 1000000L);
    return 0;
}

static int ai_state_is_fresh(const char *ai)
{
    char published_text[32];
    char *end;
    unsigned long long now;
    unsigned long long published;

    if (json_number(ai, "monotonic_ms", published_text,
                    sizeof(published_text)) != 0 ||
        monotonic_ms(&now) != 0)
        return 0;

    errno = 0;
    published = strtoull(published_text, &end, 10);
    while (isspace((unsigned char)*end) != 0)
        ++end;
    if (errno != 0 || end == published_text || *end != '\0' ||
        now < published)
        return 0;

    return now - published <= AI_HEALTH_TIMEOUT_MS;
}

static int write_atomic(const char *ai, const char *safety, int ai_healthy)
{
    char camera[32] = "unknown";
    char ai_state[32] = "unknown";
    char preview_transport[32] = "unavailable";
    char preview_stream[32] = "unavailable";
    char roi_active[8] = "false";
    char roi_events[32] = "0";
    char roi_clears[32] = "0";
    char preview_path[256] = "hmi_runtime/preview/latest.jpg";
    char preview_sequence[32] = "0";
    char frames_inferred[32] = "0";
    char preview_saved[32] = "0";
    char preview_failed[32] = "0";
    char last_event_source[32] = "none";
    char roi_active_bool[8] = "false";
    char preview_fps[32] = "0.0";
    char preview_bitrate[32] = "0.0";
    char encode_failures[32] = "0";
    char preview_latency[32] = "0.0";
    char alarm[4096] = "{\"state\":\"unknown\"}";
    char ld2410[4096] = "{\"state\":\"unknown\"}";
    char sht3x[4096] = "{\"state\":\"unknown\"}";
    char bh1750[4096] = "{\"state\":\"unknown\",\"value_lux\":null,\"sampled_monotonic_ms\":null}";
    char door[4096] = "{\"state\":\"unknown\",\"logical_state\":\"unknown\",\"alarm\":null,\"event_type\":null,\"event_source\":null,\"event_sequence\":null,\"event_timestamp_ns\":null,\"sampled_monotonic_ms\":null,\"age_ms\":null,\"quality\":\"not_seen\"}";
    FILE *file;
    struct timespec now;
    unsigned long long monotonic_ms = 0ULL;

    (void)json_string(ai, "camera_state", camera, sizeof(camera));
    (void)json_string(ai, "ai_state", ai_state, sizeof(ai_state));
    (void)json_string(ai, "preview_transport", preview_transport, sizeof(preview_transport));
    (void)json_string(ai, "preview_stream_state", preview_stream, sizeof(preview_stream));
    (void)json_number(ai, "roi_active", roi_active, sizeof(roi_active));
    (void)json_number(ai, "preview_frame_sequence", preview_sequence, sizeof(preview_sequence));
    (void)json_number(ai, "frames_inferred", frames_inferred, sizeof(frames_inferred));
    (void)json_number(ai, "preview_saved", preview_saved, sizeof(preview_saved));
    (void)json_number(ai, "preview_failed", preview_failed, sizeof(preview_failed));
    (void)json_string(ai, "preview_frame_path", preview_path, sizeof(preview_path));
    (void)json_string(ai, "last_event_source", last_event_source, sizeof(last_event_source));
    if (strcmp(roi_active, "true") == 0)
        snprintf(roi_active_bool, sizeof(roi_active_bool), "true");
    (void)json_number(ai, "roi_event_count", roi_events, sizeof(roi_events));
    (void)json_number(ai, "roi_clear_count", roi_clears, sizeof(roi_clears));
    (void)json_number(ai, "preview_fps", preview_fps, sizeof(preview_fps));
    (void)json_number(ai, "preview_bitrate_bps", preview_bitrate, sizeof(preview_bitrate));
    (void)json_number(ai, "preview_encode_failures", encode_failures, sizeof(encode_failures));
    (void)json_number(ai, "preview_latency_ms", preview_latency, sizeof(preview_latency));
    if (!ai_healthy) {
        snprintf(camera, sizeof(camera), "offline");
        snprintf(ai_state, sizeof(ai_state), "offline");
        snprintf(preview_transport, sizeof(preview_transport), "unavailable");
        snprintf(preview_stream, sizeof(preview_stream), "unavailable");
        snprintf(roi_active_bool, sizeof(roi_active_bool), "false");
    }
    (void)json_object(safety, "alarm", alarm, sizeof(alarm));
    (void)json_object(safety, "ld2410", ld2410, sizeof(ld2410));
    (void)json_object(safety, "sht3x", sht3x, sizeof(sht3x));
    (void)json_object(safety, "bh1750", bh1750, sizeof(bh1750));
    (void)json_object(safety, "door", door, sizeof(door));
    if (clock_gettime(CLOCK_MONOTONIC, &now) == 0)
        monotonic_ms = (unsigned long long)now.tv_sec * 1000ULL +
                       (unsigned long long)(now.tv_nsec / 1000000L);

    if (mkdir(OUTPUT_DIR, 0755) < 0 && errno != EEXIST)
        return -1;
    file = fopen(TEMP_PATH, "w");
    if (file == NULL)
        return -1;
    fprintf(file,
            "{\"schema_version\":1,\"monotonic_ms\":%llu,"
            "\"camera_state\":\"%s\",\"ai_state\":\"%s\","
            "\"preview_frame_path\":\"%s\",\"preview_frame_sequence\":%s,"
            "\"frames_inferred\":%s,\"preview_saved\":%s,\"preview_failed\":%s,"
            "\"roi_event_count\":%s,\"roi_clear_count\":%s,\"roi_active\":%s,"
            "\"last_event_source\":\"%s\",\"preview_transport\":\"%s\","
            "\"preview_stream_state\":\"%s\",\"preview_fps\":%s,\"preview_latency_ms\":%s,"
            "\"ai\":{\"state\":\"%s\",\"camera_state\":\"%s\","
            "\"roi_active\":%s,\"roi_event_count\":%s,\"roi_clear_count\":%s},"
            "\"preview\":{\"state\":\"%s\",\"transport\":\"%s\","
            "\"fps\":%s,\"bitrate_bps\":%s,\"encode_failures\":%s,\"latency_ms\":%s},"
            "\"alarm\":%s,\"ld2410\":%s,\"sht3x\":%s,\"bh1750\":%s,\"door\":%s}\n",
            monotonic_ms, camera, ai_state, preview_path, preview_sequence,
            frames_inferred, preview_saved, preview_failed, roi_events, roi_clears,
            roi_active_bool, last_event_source, preview_transport, preview_stream,
            preview_fps, preview_latency, ai_state, camera, roi_active_bool, roi_events, roi_clears,
            preview_stream, preview_transport, preview_fps, preview_bitrate,
            encode_failures, preview_latency, alarm, ld2410, sht3x, bh1750, door);
    if (fflush(file) != 0 || fsync(fileno(file)) != 0 || fclose(file) != 0)
        return -1;
    return rename(TEMP_PATH, OUTPUT_PATH);
}

int main(void)
{
    char ai[BUFFER_SIZE];
    char safety[BUFFER_SIZE];
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = request_stop;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);

    while (!stop_requested) {
        int ai_read_ok;

        memset(ai, 0, sizeof(ai));
        ai_read_ok = read_file(AI_STATE_PATH, ai, sizeof(ai)) == 0;
        if (request_snapshot(safety, sizeof(safety)) == 0)
            (void)write_atomic(ai, safety,
                               ai_read_ok && ai_state_is_fresh(ai));
        sleep(1);
    }
    unlink(TEMP_PATH);
    return 0;
}
