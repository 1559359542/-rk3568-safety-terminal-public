#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <syslog.h>
#include <asm/ioctls.h>
#include <asm/termbits.h>
#include <time.h>
#include <unistd.h>

#include "../../drivers/safety_event/safety_event_uapi.h"

#define SAFETY_EVENT_DEVICE "/dev/safety_event"

#define HWMON_CLASS_DIR "/sys/class/hwmon"
#define SHT3X_HWMON_NAME "sht3x"
#define SHT3X_TEMP_INPUT "temp1_input"
#define SHT3X_HUMIDITY_INPUT "humidity1_input"
#define SHT3X_SAMPLE_INTERVAL_MS 1000
#define BH1750_SAMPLE_STALE_MS 3000U

#define LD2410B_DEVICE "/dev/ttyS9"
#define LD2410B_READ_BUFFER_SIZE 256U
#define LD2410B_FRAME_BUFFER_SIZE 512U
#define LD2410B_CONFIG_BUFFER_SIZE 512U
#define LD2410B_NORMAL_PAYLOAD_LENGTH 13U
#define LD2410B_CONFIG_TIMEOUT_MS 1500
#define LD2410B_CONFIG_WRITE_TIMEOUT_MS 1000
#define LD2410B_SAMPLE_STALE_MS 2000U
#define LD2410B_DIAGNOSTIC_PREFIX_LENGTH 13U

#define LD2410B_STATE_NONE 0U
#define LD2410B_STATE_CALIBRATING 4U
#define LD2410B_STATE_CALIBRATION_SUCCESS 5U
#define LD2410B_STATE_CALIBRATION_FAILED 6U

#define LD2410B_NEAR_DISTANCE_CM 200U
#define LD2410B_NEAR_HOLD_SECONDS 2
#define LD2410B_NOBODY_HOLD_SECONDS 5

#define LD2410B_CMD_ENABLE_CONFIG 0x00ffU
#define LD2410B_CMD_DISABLE_CONFIG 0x00feU
#define LD2410B_CMD_START_CALIBRATION 0x000bU
#define LD2410B_CMD_QUERY_CALIBRATION 0x001bU
#define LD2410B_CMD_SET_PARAMETERS 0x0060U
#define LD2410B_CMD_READ_PARAMETERS 0x0061U
#define LD2410B_CMD_RESTART 0x00a3U
#define LD2410B_CMD_SET_RESOLUTION 0x00aaU
#define LD2410B_CMD_QUERY_RESOLUTION 0x00abU
#define LD2410B_MAX_GATE 8U
#define LD2410B_MIN_CONFIG_GATE 2U
#define LD2410B_MIN_NOBODY_SECONDS 1U
#define LD2410B_MAX_NOBODY_SECONDS 60U
#define LD2410B_RESOLUTION_750_MM 750U
#define LD2410B_RESOLUTION_200_MM 200U
#define LD2410B_RESOLUTION_750_INDEX 0U
#define LD2410B_RESOLUTION_200_INDEX 1U
#define LD2410B_RESTART_SETTLE_MS 2000U
#define CALIBRATION_RECORD_PATH "/run/safety_alarmd.calibration"

#define CONTROL_SOCKET_PATH "/run/safety_alarmd.sock"
#define CONTROL_REQUEST_SIZE 64U
#define CONTROL_RESPONSE_SIZE 2048U
#define CONTROL_BACKLOG 4

enum calibration_state {
    CALIBRATION_IDLE = 0,
    CALIBRATION_WAITING,
    CALIBRATION_RUNNING,
    CALIBRATION_SUCCESS,
    CALIBRATION_FAILED,
    CALIBRATION_UNKNOWN,
};

struct radar_parameters {
    int valid;
    uint8_t maximum_gate;
    uint8_t moving_maximum_gate;
    uint8_t stationary_maximum_gate;
    uint8_t moving_sensitivity[LD2410B_MAX_GATE + 1U];
    uint8_t stationary_sensitivity[LD2410B_MAX_GATE + 1U];
    uint16_t nobody_seconds;
};

struct rear_monitor {
    int enabled;
    int user_alarm_on;          /* 雷达请求的告警状态 */
    int ai_alarm_on;            /* AI 桥接程序请求的告警状态 */
    int applied_user_alarm_on;  /* 已写入驱动的 radar OR ai 结果 */
    int radar_sample_valid;
    int radar_sample_stale_reported;
    uint8_t latest_report_mode;
    uint8_t latest_radar_state;
    uint16_t latest_moving_distance_cm;
    uint8_t latest_moving_energy;
    uint16_t latest_stationary_distance_cm;
    uint8_t latest_stationary_energy;
    uint16_t latest_detection_distance_cm;
    size_t latest_payload_length;
    uint8_t latest_payload_prefix[LD2410B_DIAGNOSTIC_PREFIX_LENGTH];
    int latest_gate_energy_valid;
    uint8_t latest_moving_max_gate;
    uint8_t latest_stationary_max_gate;
    uint8_t latest_moving_gate_energy[LD2410B_MAX_GATE + 1U];
    uint8_t latest_stationary_gate_energy[LD2410B_MAX_GATE + 1U];
    uint8_t latest_light_value;
    uint8_t latest_out_state;
    uint64_t radar_frame_count;
    struct timespec latest_radar_sample_time;
    int bh1750_sample_valid;
    uint32_t latest_bh1750_lux;
    struct timespec latest_bh1750_sample_time;
    int door_sample_valid;
    uint32_t latest_door_sequence;
    uint32_t latest_door_state;
    uint64_t latest_door_event_timestamp_ns;
    struct timespec latest_door_sample_time;
    int near_active;
    int nobody_active;
    enum calibration_state calibration;
    unsigned int calibration_seconds;
    int parameters_changed;
    int snapshot_after_pending;
    int record_pending;
    struct radar_parameters parameters_before;
    struct radar_parameters parameters_after;
    struct timespec near_since;
    struct timespec nobody_since;
};

static const uint8_t ld2410b_data_header[] = {
    0xf4U, 0xf3U, 0xf2U, 0xf1U,
};

static const uint8_t ld2410b_data_footer[] = {
    0xf8U, 0xf7U, 0xf6U, 0xf5U,
};

static const uint8_t ld2410b_command_header[] = {
    0xfdU, 0xfcU, 0xfbU, 0xfaU,
};

static const uint8_t ld2410b_command_footer[] = {
    0x04U, 0x03U, 0x02U, 0x01U,
};

static volatile sig_atomic_t stop_requested;
static int sht3x_online = -1;
static long sht3x_temperature_mc;
static long sht3x_humidity_mper;
static uint64_t sht3x_sample_monotonic_ms;

static int read_radar_parameters(int uart_fd,
                                 struct radar_parameters *parameters);
static int radar_sample_age_ms(const struct rear_monitor *monitor,
                               uint64_t *age_ms);
static int bh1750_sample_age_ms(const struct rear_monitor *monitor,
                                uint64_t *age_ms);
static const char *calibration_state_name(enum calibration_state state);
static int set_radar_nobody_seconds(int uart_fd,
                                    struct rear_monitor *monitor,
                                    unsigned int seconds);
static int read_radar_resolution(int uart_fd, unsigned int *resolution_mm);
static int set_radar_resolution(int uart_fd, struct rear_monitor *monitor,
                                unsigned int resolution_mm,
                                int *restarted);
static int set_radar_maximum_gate(int uart_fd,
                                  struct rear_monitor *monitor,
                                  unsigned int maximum_gate);
static void write_calibration_record(const struct rear_monitor *monitor);
static void load_calibration_record(struct rear_monitor *monitor);
static void finish_calibration_if_needed(int uart_fd,
                                         struct rear_monitor *monitor);

static void request_stop(int signum)
{
    (void)signum;
    stop_requested = 1;
}

static int read_long_file(const char *path, long *value)
{
    FILE *file = fopen(path, "r");

    if (!file) {
        syslog(LOG_ERR, "open %s failed: %s", path, strerror(errno));
        return -1;
    }

    if (fscanf(file, "%ld", value) != 1) {
        syslog(LOG_ERR, "read integer from %s failed", path);
        fclose(file);
        return -1;
    }

    if (fclose(file) != 0) {
        syslog(LOG_ERR, "close %s failed: %s", path, strerror(errno));
        return -1;
    }

    return 0;
}

static int discover_sht3x_hwmon(char *directory, size_t directory_size)
{
    DIR *class_dir;
    struct dirent *entry;

    class_dir = opendir(HWMON_CLASS_DIR);
    if (!class_dir) {
        syslog(LOG_ERR, "open %s failed: %s", HWMON_CLASS_DIR,
               strerror(errno));
        return -1;
    }

    while ((entry = readdir(class_dir)) != NULL) {
        char name_path[PATH_MAX];
        char name[64];
        FILE *name_file;
        int length;

        if (strncmp(entry->d_name, "hwmon", 5) != 0)
            continue;

        length = snprintf(name_path, sizeof(name_path), "%s/%s/name",
                          HWMON_CLASS_DIR, entry->d_name);
        if (length < 0 || (size_t)length >= sizeof(name_path))
            continue;

        name_file = fopen(name_path, "r");
        if (!name_file)
            continue;

        if (!fgets(name, sizeof(name), name_file)) {
            fclose(name_file);
            continue;
        }
        fclose(name_file);

        name[strcspn(name, "\r\n")] = '\0';
        if (strcmp(name, SHT3X_HWMON_NAME) != 0)
            continue;

        length = snprintf(directory, directory_size, "%s/%s",
                          HWMON_CLASS_DIR, entry->d_name);
        closedir(class_dir);
        if (length < 0 || (size_t)length >= directory_size)
            return -1;
        return 0;
    }

    closedir(class_dir);
    return -1;
}

static void sample_sht3x(void)
{
    char directory[PATH_MAX];
    char temp_path[PATH_MAX];
    char humidity_path[PATH_MAX];
    long temp_mc;
    long humidity_mper;
    int length;

    if (discover_sht3x_hwmon(directory, sizeof(directory)) < 0)
        goto offline;

    length = snprintf(temp_path, sizeof(temp_path), "%s/%s", directory,
                      SHT3X_TEMP_INPUT);
    if (length < 0 || (size_t)length >= sizeof(temp_path))
        goto offline;

    length = snprintf(humidity_path, sizeof(humidity_path), "%s/%s",
                      directory, SHT3X_HUMIDITY_INPUT);
    if (length < 0 || (size_t)length >= sizeof(humidity_path))
        goto offline;

    if (read_long_file(temp_path, &temp_mc) < 0 ||
        read_long_file(humidity_path, &humidity_mper) < 0)
        goto offline;

    if (sht3x_online != 1)
        syslog(LOG_INFO, "sht3x online hwmon=%s", directory);
    sht3x_temperature_mc = temp_mc;
    sht3x_humidity_mper = humidity_mper;
    {
        struct timespec now;

        if (clock_gettime(CLOCK_MONOTONIC, &now) == 0)
            sht3x_sample_monotonic_ms = (uint64_t)now.tv_sec * 1000ULL +
                                        (uint64_t)(now.tv_nsec / 1000000L);
    }
    sht3x_online = 1;
    return;

offline:
    if (sht3x_online != 0)
        syslog(LOG_ERR, "sht3x offline");
    sht3x_online = 0;
}

static const char *radar_state_name(const struct rear_monitor *monitor,
                                    uint64_t sample_age_ms)
{
    if (!monitor->radar_sample_valid)
        return "unknown";
    if (sample_age_ms > LD2410B_SAMPLE_STALE_MS)
        return "stale";
    return monitor->latest_radar_state == LD2410B_STATE_NONE ? "none" : "target";
}

static int format_snapshot_response(const struct rear_monitor *monitor,
                                    char *response, size_t response_size)
{
    struct timespec now;
    uint64_t sample_age_ms = 0;
    uint64_t bh1750_age_ms = 0;
    uint64_t door_age_ms = 0;
    uint64_t monotonic_ms = 0;
    const int radar_age_ok = radar_sample_age_ms(monitor, &sample_age_ms) == 0;
    const int radar_online = monitor->radar_sample_valid && radar_age_ok;
    const int bh1750_age_ok = bh1750_sample_age_ms(monitor, &bh1750_age_ms) == 0;
    const int bh1750_online = monitor->bh1750_sample_valid && bh1750_age_ok &&
                              bh1750_age_ms <= BH1750_SAMPLE_STALE_MS;
    int door_age_ok = 0;
    const char *radar_state = radar_state_name(monitor, sample_age_ms);
    const char *sht_state = sht3x_online == 1 ? "online" :
                            (sht3x_online == 0 ? "offline" : "unknown");
    const char *bh1750_state = bh1750_online ? "online" :
                               (monitor->bh1750_sample_valid ? "stale" : "unknown");
    char age_text[32];
    char detect_text[32];
    char move_text[32];
    char move_energy_text[32];
    char temperature_text[32];
    char humidity_text[32];
    char sampled_text[32];
    char bh1750_lux_text[32];
    char bh1750_sampled_text[32];
    char door_state_text[32];
    char door_logical_text[32];
    char door_alarm_text[16];
    char door_event_type_text[16];
    char door_event_source_text[16];
    char door_sequence_text[32];
    char door_event_timestamp_text[32];
    char door_sampled_text[32];
    char door_age_text[32];
    char door_quality_text[32];
    int length;

    if (clock_gettime(CLOCK_MONOTONIC, &now) == 0)
        monotonic_ms = (uint64_t)now.tv_sec * 1000ULL +
                       (uint64_t)(now.tv_nsec / 1000000L);
    if (radar_online) {
        snprintf(age_text, sizeof(age_text), "%llu", (unsigned long long)sample_age_ms);
        snprintf(detect_text, sizeof(detect_text), "%u",
                 (unsigned int)monitor->latest_detection_distance_cm);
        snprintf(move_text, sizeof(move_text), "%u",
                 (unsigned int)monitor->latest_moving_distance_cm);
        snprintf(move_energy_text, sizeof(move_energy_text), "%u",
                 (unsigned int)monitor->latest_moving_energy);
    } else {
        snprintf(age_text, sizeof(age_text), "null");
        snprintf(detect_text, sizeof(detect_text), "null");
        snprintf(move_text, sizeof(move_text), "null");
        snprintf(move_energy_text, sizeof(move_energy_text), "null");
    }
    if (sht3x_online == 1) {
        snprintf(temperature_text, sizeof(temperature_text), "%.3f",
                 (double)sht3x_temperature_mc / 1000.0);
        snprintf(humidity_text, sizeof(humidity_text), "%.3f",
                 (double)sht3x_humidity_mper / 1000.0);
        snprintf(sampled_text, sizeof(sampled_text), "%llu",
                 (unsigned long long)sht3x_sample_monotonic_ms);
    } else {
        snprintf(temperature_text, sizeof(temperature_text), "null");
        snprintf(humidity_text, sizeof(humidity_text), "null");
        snprintf(sampled_text, sizeof(sampled_text), "null");
    }
    if (bh1750_online) {
        snprintf(bh1750_lux_text, sizeof(bh1750_lux_text), "%u",
                 monitor->latest_bh1750_lux);
        snprintf(bh1750_sampled_text, sizeof(bh1750_sampled_text), "%llu",
                 (unsigned long long)((uint64_t)monitor->latest_bh1750_sample_time.tv_sec * 1000ULL +
                                      (uint64_t)(monitor->latest_bh1750_sample_time.tv_nsec / 1000000L)));
    } else {
        snprintf(bh1750_lux_text, sizeof(bh1750_lux_text), "null");
        snprintf(bh1750_sampled_text, sizeof(bh1750_sampled_text), "null");
    }
    if (monitor->door_sample_valid &&
        clock_gettime(CLOCK_MONOTONIC, &now) == 0) {
        time_t seconds = now.tv_sec - monitor->latest_door_sample_time.tv_sec;
        long nanoseconds = now.tv_nsec -
                           monitor->latest_door_sample_time.tv_nsec;

        if (nanoseconds < 0) {
            --seconds;
            nanoseconds += 1000000000L;
        }
        if (seconds >= 0) {
            door_age_ms = (uint64_t)seconds * 1000ULL +
                          (uint64_t)(nanoseconds / 1000000L);
            door_age_ok = 1;
        }
    }
    if (!monitor->door_sample_valid) {
        snprintf(door_state_text, sizeof(door_state_text), "unknown");
        snprintf(door_logical_text, sizeof(door_logical_text), "unknown");
        snprintf(door_alarm_text, sizeof(door_alarm_text), "null");
        snprintf(door_event_type_text, sizeof(door_event_type_text), "null");
        snprintf(door_event_source_text, sizeof(door_event_source_text), "null");
        snprintf(door_sequence_text, sizeof(door_sequence_text), "null");
        snprintf(door_event_timestamp_text, sizeof(door_event_timestamp_text), "null");
        snprintf(door_sampled_text, sizeof(door_sampled_text), "null");
        snprintf(door_age_text, sizeof(door_age_text), "null");
        snprintf(door_quality_text, sizeof(door_quality_text), "not_seen");
    } else {
        snprintf(door_state_text, sizeof(door_state_text),
                 door_age_ok ? "online" : "unknown");
        snprintf(door_logical_text, sizeof(door_logical_text),
                 monitor->latest_door_state == SAFETY_EVENT_STATE_LOGICAL_ACTIVE ?
                 "active" :
                 (monitor->latest_door_state == SAFETY_EVENT_STATE_LOGICAL_INACTIVE ?
                  "inactive" : "unknown"));
        snprintf(door_alarm_text, sizeof(door_alarm_text),
                 monitor->latest_door_state == SAFETY_EVENT_STATE_LOGICAL_ACTIVE ?
                 "true" : "false");
        snprintf(door_event_type_text, sizeof(door_event_type_text), "4");
        snprintf(door_event_source_text, sizeof(door_event_source_text), "2");
        snprintf(door_sequence_text, sizeof(door_sequence_text), "%u",
                 monitor->latest_door_sequence);
        snprintf(door_event_timestamp_text, sizeof(door_event_timestamp_text),
                 "%llu", (unsigned long long)monitor->latest_door_event_timestamp_ns);
        snprintf(door_sampled_text, sizeof(door_sampled_text), "%llu",
                 (unsigned long long)((uint64_t)monitor->latest_door_sample_time.tv_sec * 1000ULL +
                                      (uint64_t)(monitor->latest_door_sample_time.tv_nsec / 1000000L)));
        snprintf(door_age_text, sizeof(door_age_text), "%llu",
                 (unsigned long long)door_age_ms);
        snprintf(door_quality_text, sizeof(door_quality_text),
                 door_age_ok ? "event_seen" : "source_error");
    }

    length = snprintf(response, response_size,
                      "{\"schema_version\":1,\"monotonic_ms\":%llu,"
                      "\"alarm\":{\"state\":\"online\",\"user_alarm\":%s,"
                      "\"radar_alarm\":%s,\"ai_alarm\":%s,\"applied_alarm\":%s},"
                      "\"ld2410\":{\"state\":\"%s\",\"frame_count\":%llu,"
                      "\"age_ms\":%s,\"radar_state\":\"%s\","
                      "\"detect_cm\":%s,\"move_cm\":%s,\"move_energy\":%s,"
                      "\"calibration\":\"%s\"},"
                      "\"sht3x\":{\"state\":\"%s\","
                      "\"temperature_c\":%s,\"humidity_rh\":%s,"
                      "\"sampled_monotonic_ms\":%s},"
                      "\"bh1750\":{\"state\":\"%s\","
                      "\"value_lux\":%s,\"sampled_monotonic_ms\":%s},"
                      "\"door\":{\"state\":\"%s\",\"logical_state\":\"%s\","
                      "\"alarm\":%s,\"event_type\":%s,\"event_source\":%s,"
                      "\"event_sequence\":%s,\"event_timestamp_ns\":%s,"
                      "\"sampled_monotonic_ms\":%s,\"age_ms\":%s,"
                      "\"quality\":\"%s\"}}\n",
                      (unsigned long long)monotonic_ms,
                      monitor->user_alarm_on ? "true" : "false",
                      monitor->user_alarm_on ? "true" : "false",
                      monitor->ai_alarm_on ? "true" : "false",
                      monitor->applied_user_alarm_on ? "true" : "false",
                      radar_online ? "online" : (monitor->radar_sample_valid ? "stale" : "unknown"),
                      (unsigned long long)monitor->radar_frame_count,
                      age_text, radar_state, detect_text, move_text, move_energy_text,
                      calibration_state_name(monitor->calibration), sht_state,
                      temperature_text, humidity_text, sampled_text,
                      bh1750_state, bh1750_lux_text, bh1750_sampled_text,
                      door_state_text, door_logical_text, door_alarm_text,
                      door_event_type_text, door_event_source_text,
                      door_sequence_text, door_event_timestamp_text,
                      door_sampled_text, door_age_text, door_quality_text);

    if (length < 0 || (size_t)length >= response_size)
        return -1;

    return length;
}

static int check_api_version(int fd)
{
    __u32 version = 0;

    if (ioctl(fd, SAFETY_EVENT_IOC_GET_API_VERSION, &version) < 0) {
        syslog(LOG_ERR, "GET_API_VERSION failed: %s", strerror(errno));
        return -1;
    }

    syslog(LOG_INFO, "api_version=%u", version);
    if (version != SAFETY_EVENT_API_VERSION) {
        syslog(LOG_ERR, "API version mismatch, expected %u",
               SAFETY_EVENT_API_VERSION);
        return -1;
    }

    return 0;
}

static int elapsed_at_least(const struct timespec *start,
                            const struct timespec *now,
                            time_t seconds)
{
    time_t elapsed_seconds;
    long elapsed_nanoseconds;

    elapsed_seconds = now->tv_sec - start->tv_sec;
    elapsed_nanoseconds = now->tv_nsec - start->tv_nsec;

    if (elapsed_nanoseconds < 0) {
        --elapsed_seconds;
        elapsed_nanoseconds += 1000000000L;
    }

    return elapsed_seconds >= seconds;
}

static int radar_sample_age_ms(const struct rear_monitor *monitor,
                               uint64_t *age_ms)
{
    struct timespec now;
    time_t seconds;
    long nanoseconds;

    if (!monitor->radar_sample_valid ||
        clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return -1;

    seconds = now.tv_sec - monitor->latest_radar_sample_time.tv_sec;
    nanoseconds = now.tv_nsec - monitor->latest_radar_sample_time.tv_nsec;
    if (nanoseconds < 0) {
        --seconds;
        nanoseconds += 1000000000L;
    }
    if (seconds < 0)
        return -1;

    *age_ms = (uint64_t)seconds * 1000U +
              (uint64_t)nanoseconds / 1000000U;
    return 0;
}

static int bh1750_sample_age_ms(const struct rear_monitor *monitor,
                                uint64_t *age_ms)
{
    struct timespec now;
    time_t seconds;
    long nanoseconds;

    if (!monitor->bh1750_sample_valid ||
        clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return -1;

    seconds = now.tv_sec - monitor->latest_bh1750_sample_time.tv_sec;
    nanoseconds = now.tv_nsec - monitor->latest_bh1750_sample_time.tv_nsec;
    if (nanoseconds < 0) {
        --seconds;
        nanoseconds += 1000000000L;
    }
    if (seconds < 0)
        return -1;

    *age_ms = (uint64_t)seconds * 1000U +
              (uint64_t)nanoseconds / 1000000U;
    return 0;
}

static int radar_sample_is_fresh(const struct rear_monitor *monitor)
{
    uint64_t age_ms;

    return radar_sample_age_ms(monitor, &age_ms) == 0 &&
           age_ms <= LD2410B_SAMPLE_STALE_MS;
}

static int format_hex_bytes(const uint8_t *data, size_t length,
                            char *buffer, size_t buffer_size)
{
    size_t index;
    size_t offset = 0;

    if (buffer_size == 0U)
        return -1;

    for (index = 0; index < length; ++index) {
        int written = snprintf(buffer + offset, buffer_size - offset,
                               "%02x", data[index]);

        if (written < 0 || (size_t)written >= buffer_size - offset)
            return -1;
        offset += (size_t)written;
    }

    return 0;
}

static int format_u8_list(const uint8_t *data, size_t length,
                          char *buffer, size_t buffer_size)
{
    size_t index;
    size_t offset = 0;

    if (buffer_size == 0U)
        return -1;

    for (index = 0; index < length; ++index) {
        int written = snprintf(buffer + offset, buffer_size - offset,
                               index == 0U ? "%u" : ",%u",
                               (unsigned int)data[index]);

        if (written < 0 || (size_t)written >= buffer_size - offset)
            return -1;
        offset += (size_t)written;
    }

    return 0;
}

static int apply_user_alarm(int event_fd, struct rear_monitor *monitor)
{
    const int active = (monitor->user_alarm_on != 0 ||
                        monitor->ai_alarm_on != 0) ? 1 : 0;
    struct safety_event_user_alarm_request request = {
        .active = active ? 1U : 0U,
        .reserved = 0U,
    };

    if (monitor->applied_user_alarm_on == active)
        return 0;

    if (ioctl(event_fd, SAFETY_EVENT_IOC_SET_USER_ALARM, &request) < 0) {
        syslog(LOG_ERR, "SET_USER_ALARM(%u) failed: %s",
               request.active, strerror(errno));
        return -1;
    }

    monitor->applied_user_alarm_on = active;
    syslog(LOG_INFO, "user_alarm=%u radar=%u ai=%u", request.active,
           monitor->user_alarm_on ? 1U : 0U,
           monitor->ai_alarm_on ? 1U : 0U);
    return 0;
}

static int set_radar_user_alarm(int event_fd, struct rear_monitor *monitor,
                                int active)
{
    monitor->user_alarm_on = active ? 1 : 0;
    return apply_user_alarm(event_fd, monitor);
}

static int set_ai_user_alarm(int event_fd, struct rear_monitor *monitor,
                             int active)
{
    monitor->ai_alarm_on = active ? 1 : 0;
    return apply_user_alarm(event_fd, monitor);
}

static int clear_all_user_alarms(int event_fd, struct rear_monitor *monitor)
{
    monitor->user_alarm_on = 0;
    monitor->ai_alarm_on = 0;
    return apply_user_alarm(event_fd, monitor);
}

static void clear_rear_timers(struct rear_monitor *monitor)
{
    monitor->near_active = 0;
    monitor->nobody_active = 0;
    memset(&monitor->near_since, 0, sizeof(monitor->near_since));
    memset(&monitor->nobody_since, 0, sizeof(monitor->nobody_since));
}

static int calibration_is_active(const struct rear_monitor *monitor)
{
    return monitor->calibration == CALIBRATION_WAITING ||
           monitor->calibration == CALIBRATION_RUNNING;
}

static int enable_rear_monitor(int event_fd, struct rear_monitor *monitor)
{
    if (calibration_is_active(monitor)) {
        syslog(LOG_WARNING, "rear enable rejected during calibration");
        return -1;
    }

    if (!radar_sample_is_fresh(monitor)) {
        syslog(LOG_WARNING, "rear enable rejected: radar sample is stale");
        return -1;
    }

    clear_rear_timers(monitor);
    monitor->enabled = 1;

    if (set_radar_user_alarm(event_fd, monitor, 0) < 0)
        return -1;

    syslog(LOG_INFO, "rear monitor enabled");
    return 0;
}

static int disable_rear_monitor(int event_fd, struct rear_monitor *monitor)
{
    clear_rear_timers(monitor);
    monitor->enabled = 0;

    /* 仅释放雷达请求；AI、门磁和低照度来源保持各自所有权。 */
    if (set_radar_user_alarm(event_fd, monitor, 0) < 0)
        return -1;

    syslog(LOG_INFO, "rear monitor disabled");
    return 0;
}

static int update_rear_monitor(int event_fd, struct rear_monitor *monitor,
                               uint8_t state, uint16_t distance_cm)
{
    struct timespec now;

    if (!monitor->enabled)
        return 0;

    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
        syslog(LOG_ERR, "clock_gettime failed: %s", strerror(errno));
        return -1;
    }

    if (state == LD2410B_STATE_NONE ||
        distance_cm > LD2410B_NEAR_DISTANCE_CM) {
        monitor->near_active = 0;

        if (!monitor->user_alarm_on) {
            monitor->nobody_active = 0;
            return 0;
        }

        if (!monitor->nobody_active) {
            monitor->nobody_active = 1;
            monitor->nobody_since = now;
            syslog(LOG_INFO,
                   "radar channel-clear timer started state=%u distance_cm=%u",
                   state, distance_cm);
        }

        if (elapsed_at_least(&monitor->nobody_since, &now,
                             LD2410B_NOBODY_HOLD_SECONDS)) {
            if (set_radar_user_alarm(event_fd, monitor, 0) < 0)
                return -1;
        }

        return 0;
    }

    monitor->nobody_active = 0;

    if (!monitor->near_active) {
        monitor->near_active = 1;
        monitor->near_since = now;
        syslog(LOG_INFO, "radar near timer started state=%u distance_cm=%u",
               state, distance_cm);
        return 0;
    }

    if (elapsed_at_least(&monitor->near_since, &now,
                         LD2410B_NEAR_HOLD_SECONDS)) {
        if (set_radar_user_alarm(event_fd, monitor, 1) < 0)
            return -1;
    }

    return 0;
}

static int set_uart_256000(int fd)
{
    struct termios2 settings;

    if (ioctl(fd, TCGETS2, &settings) < 0) {
        syslog(LOG_ERR, "TCGETS2 %s failed: %s",
               LD2410B_DEVICE, strerror(errno));
        return -1;
    }

    settings.c_iflag = 0;
    settings.c_oflag = 0;
    settings.c_lflag = 0;
    settings.c_cflag &= (tcflag_t)~(CBAUD | PARENB | CSTOPB | CSIZE);
    settings.c_cflag |= BOTHER | CLOCAL | CREAD | CS8;
    settings.c_cc[VMIN] = 0;
    settings.c_cc[VTIME] = 0;
    settings.c_ispeed = 256000U;
    settings.c_ospeed = 256000U;

    if (ioctl(fd, TCSETS2, &settings) < 0) {
        syslog(LOG_ERR, "TCSETS2 %s failed: %s",
               LD2410B_DEVICE, strerror(errno));
        return -1;
    }

    return 0;
}

static int open_ld2410b_uart(void)
{
    int fd;

    fd = open(LD2410B_DEVICE, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        syslog(LOG_ERR, "open %s failed: %s",
               LD2410B_DEVICE, strerror(errno));
        return -1;
    }

    if (set_uart_256000(fd) < 0) {
        close(fd);
        return -1;
    }
    syslog(LOG_INFO, "LD2410B UART opened: %s", LD2410B_DEVICE);
    return fd;
}

static int header_matches(const uint8_t *data, const uint8_t *header,
                          size_t header_size)
{
    return memcmp(data, header, header_size) == 0;
}

static int write_all(int fd, const uint8_t *data, size_t length)
{
    size_t written = 0;

    while (written < length) {
        ssize_t bytes = write(fd, data + written, length - written);

        if (bytes > 0) {
            written += (size_t)bytes;
            continue;
        }

        if (bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd pfd = {
                .fd = fd,
                .events = POLLOUT,
            };

            if (poll(&pfd, 1, LD2410B_CONFIG_WRITE_TIMEOUT_MS) <= 0) {
                syslog(LOG_ERR, "UART command write timed out");
                return -1;
            }
            continue;
        }

        syslog(LOG_ERR, "UART command write failed: %s", strerror(errno));
        return -1;
    }

    return 0;
}

static int send_ld2410b_command(int uart_fd, uint16_t command,
                                const uint8_t *value, size_t value_length)
{
    uint8_t frame[64];
    size_t payload_length = 2U + value_length;
    size_t frame_length = 4U + 2U + payload_length + 4U;

    if (frame_length > sizeof(frame) || value_length > UINT16_MAX - 2U)
        return -1;

    memcpy(frame, ld2410b_command_header, sizeof(ld2410b_command_header));
    frame[4] = (uint8_t)(payload_length & 0xffU);
    frame[5] = (uint8_t)(payload_length >> 8);
    frame[6] = (uint8_t)(command & 0xffU);
    frame[7] = (uint8_t)(command >> 8);

    if (value_length > 0)
        memcpy(frame + 8, value, value_length);

    memcpy(frame + 8 + value_length, ld2410b_command_footer,
           sizeof(ld2410b_command_footer));

    syslog(LOG_INFO, "LD2410B command=0x%04x", command);
    return write_all(uart_fd, frame, frame_length);
}

static int wait_for_command_ack(int uart_fd, uint16_t expected_ack)
{
    uint8_t buffer[LD2410B_CONFIG_BUFFER_SIZE];
    size_t buffer_length = 0;

    for (;;) {
        struct pollfd pfd = {
            .fd = uart_fd,
            .events = POLLIN,
        };
        int poll_rc;
        ssize_t bytes;
        size_t offset = 0;

        poll_rc = poll(&pfd, 1, LD2410B_CONFIG_TIMEOUT_MS);
        if (poll_rc <= 0) {
            if (poll_rc == 0)
                syslog(LOG_ERR, "LD2410B ACK 0x%04x timed out",
                       expected_ack);
            else
                syslog(LOG_ERR, "wait for LD2410B ACK failed: %s",
                       strerror(errno));
            return -1;
        }

        bytes = read(uart_fd, buffer + buffer_length,
                     sizeof(buffer) - buffer_length);
        if (bytes < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                continue;

            syslog(LOG_ERR, "read LD2410B ACK failed: %s", strerror(errno));
            return -1;
        }

        if (bytes == 0)
            continue;

        buffer_length += (size_t)bytes;

        while (buffer_length - offset >= 10U) {
            const uint8_t *frame = buffer + offset;
            size_t payload_length;
            size_t frame_length;
            const uint8_t *payload;
            uint16_t ack_command;
            uint16_t status;

            if (!header_matches(frame, ld2410b_command_header,
                                sizeof(ld2410b_command_header))) {
                ++offset;
                continue;
            }

            payload_length = (size_t)frame[4] | ((size_t)frame[5] << 8);
            frame_length = 4U + 2U + payload_length + 4U;

            if (frame_length > sizeof(buffer)) {
                ++offset;
                continue;
            }

            if (buffer_length - offset < frame_length)
                break;

            if (!header_matches(frame + frame_length - 4U,
                                ld2410b_command_footer,
                                sizeof(ld2410b_command_footer))) {
                ++offset;
                continue;
            }

            payload = frame + 6U;
            if (payload_length >= 4U) {
                ack_command = (uint16_t)payload[0] |
                              ((uint16_t)payload[1] << 8);
                status = (uint16_t)payload[2] |
                         ((uint16_t)payload[3] << 8);

                if (ack_command == expected_ack) {
                    if (status != 0U) {
                        syslog(LOG_ERR,
                               "LD2410B ACK 0x%04x failed status=%u",
                               expected_ack, status);
                        return -1;
                    }

                    syslog(LOG_INFO, "LD2410B ACK 0x%04x success",
                           expected_ack);
                    return 0;
                }
            }

            offset += frame_length;
        }

        if (offset > 0) {
            memmove(buffer, buffer + offset, buffer_length - offset);
            buffer_length -= offset;
        }

        if (buffer_length == sizeof(buffer))
            buffer_length = 0;
    }
}

static int wait_for_u16_value_ack(int uart_fd, uint16_t expected_ack,
                                  uint16_t *value)
{
    uint8_t buffer[LD2410B_CONFIG_BUFFER_SIZE];
    size_t buffer_length = 0;

    for (;;) {
        struct pollfd pfd = {
            .fd = uart_fd,
            .events = POLLIN,
        };
        size_t offset = 0;
        ssize_t bytes;
        int poll_rc;

        poll_rc = poll(&pfd, 1, LD2410B_CONFIG_TIMEOUT_MS);
        if (poll_rc <= 0) {
            if (poll_rc == 0)
                syslog(LOG_ERR, "LD2410B value ACK 0x%04x timed out",
                       expected_ack);
            else
                syslog(LOG_ERR, "wait for LD2410B value ACK failed: %s",
                       strerror(errno));
            return -1;
        }

        bytes = read(uart_fd, buffer + buffer_length,
                     sizeof(buffer) - buffer_length);
        if (bytes < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            syslog(LOG_ERR, "read LD2410B value ACK failed: %s",
                   strerror(errno));
            return -1;
        }
        if (bytes == 0)
            continue;

        buffer_length += (size_t)bytes;
        while (buffer_length - offset >= 10U) {
            const uint8_t *frame = buffer + offset;
            const uint8_t *payload;
            size_t payload_length;
            size_t frame_length;
            uint16_t ack_command;
            uint16_t ack_status;

            if (!header_matches(frame, ld2410b_command_header,
                                sizeof(ld2410b_command_header))) {
                ++offset;
                continue;
            }

            payload_length = (size_t)frame[4] | ((size_t)frame[5] << 8);
            frame_length = 6U + payload_length + 4U;
            if (frame_length > sizeof(buffer)) {
                ++offset;
                continue;
            }
            if (buffer_length - offset < frame_length)
                break;
            if (!header_matches(frame + frame_length - 4U,
                                ld2410b_command_footer,
                                sizeof(ld2410b_command_footer))) {
                ++offset;
                continue;
            }

            payload = frame + 6U;
            if (payload_length >= 6U) {
                ack_command = (uint16_t)payload[0] |
                              ((uint16_t)payload[1] << 8);
                ack_status = (uint16_t)payload[2] |
                             ((uint16_t)payload[3] << 8);
                if (ack_command == expected_ack) {
                    if (ack_status != 0U) {
                        syslog(LOG_ERR,
                               "LD2410B value ACK 0x%04x failed status=%u",
                               expected_ack, ack_status);
                        return -1;
                    }
                    *value = (uint16_t)payload[4] |
                             ((uint16_t)payload[5] << 8);
                    return 0;
                }
            }

            offset += frame_length;
        }

        if (offset > 0) {
            memmove(buffer, buffer + offset, buffer_length - offset);
            buffer_length -= offset;
        }
        if (buffer_length == sizeof(buffer))
            buffer_length = 0;
    }
}

static int wait_for_calibration_status_ack(int uart_fd,
                                           unsigned int *calibration_state)
{
    uint8_t buffer[LD2410B_CONFIG_BUFFER_SIZE];
    size_t buffer_length = 0;

    for (;;) {
        struct pollfd pfd = {
            .fd = uart_fd,
            .events = POLLIN,
        };
        int poll_rc;
        ssize_t bytes;
        size_t offset = 0;

        poll_rc = poll(&pfd, 1, LD2410B_CONFIG_TIMEOUT_MS);
        if (poll_rc <= 0) {
            if (poll_rc == 0)
                syslog(LOG_ERR, "LD2410B calibration status timed out");
            else
                syslog(LOG_ERR, "wait for calibration status failed: %s",
                       strerror(errno));
            return -1;
        }

        bytes = read(uart_fd, buffer + buffer_length,
                     sizeof(buffer) - buffer_length);
        if (bytes < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                continue;

            syslog(LOG_ERR, "read calibration status failed: %s",
                   strerror(errno));
            return -1;
        }

        if (bytes == 0)
            continue;

        buffer_length += (size_t)bytes;

        while (buffer_length - offset >= 10U) {
            const uint8_t *frame = buffer + offset;
            const uint8_t *payload;
            size_t payload_length;
            size_t frame_length;
            uint16_t ack_command;
            uint16_t ack_status;

            if (!header_matches(frame, ld2410b_command_header,
                                sizeof(ld2410b_command_header))) {
                ++offset;
                continue;
            }

            payload_length = (size_t)frame[4] | ((size_t)frame[5] << 8);
            frame_length = 4U + 2U + payload_length + 4U;

            if (frame_length > sizeof(buffer)) {
                ++offset;
                continue;
            }

            if (buffer_length - offset < frame_length)
                break;

            if (!header_matches(frame + frame_length - 4U,
                                ld2410b_command_footer,
                                sizeof(ld2410b_command_footer))) {
                ++offset;
                continue;
            }

            payload = frame + 6U;
            if (payload_length >= 6U) {
                ack_command = (uint16_t)payload[0] |
                              ((uint16_t)payload[1] << 8);
                ack_status = (uint16_t)payload[2] |
                             ((uint16_t)payload[3] << 8);

                if (ack_command ==
                    (LD2410B_CMD_QUERY_CALIBRATION | 0x0100U)) {
                    if (ack_status != 0U) {
                        syslog(LOG_ERR,
                               "LD2410B calibration status ACK failed=%u",
                               ack_status);
                        return -1;
                    }

                    *calibration_state = (unsigned int)payload[4] |
                                         ((unsigned int)payload[5] << 8);
                    return 0;
                }
            }

            offset += frame_length;
        }

        if (offset > 0) {
            memmove(buffer, buffer + offset, buffer_length - offset);
            buffer_length -= offset;
        }

        if (buffer_length == sizeof(buffer))
            buffer_length = 0;
    }
}
static void end_configuration_best_effort(int uart_fd);
static int query_calibration_state(int uart_fd,
                                   unsigned int *calibration_state)
{
    if (send_ld2410b_command(uart_fd, LD2410B_CMD_ENABLE_CONFIG,
                             (const uint8_t[]){ 0x01U, 0x00U }, 2U) < 0 ||
        wait_for_command_ack(uart_fd,
                             LD2410B_CMD_ENABLE_CONFIG | 0x0100U) < 0)
        return -1;

    if (send_ld2410b_command(uart_fd, LD2410B_CMD_QUERY_CALIBRATION,
                             NULL, 0) < 0 ||
        wait_for_calibration_status_ack(uart_fd, calibration_state) < 0) {
        end_configuration_best_effort(uart_fd);
        return -1;
    }

    if (send_ld2410b_command(uart_fd, LD2410B_CMD_DISABLE_CONFIG,
                             NULL, 0) < 0 ||
        wait_for_command_ack(uart_fd,
                             LD2410B_CMD_DISABLE_CONFIG | 0x0100U) < 0)
        return -1;

    return 0;
}

static const char *module_calibration_state_name(unsigned int state)
{
    switch (state) {
    case 0U:
        return "not-running";
    case 1U:
        return "running";
    case 2U:
        return "completed";
    default:
        return "unknown";
    }
}
static void end_configuration_best_effort(int uart_fd)
{
    if (send_ld2410b_command(uart_fd, LD2410B_CMD_DISABLE_CONFIG,
                             NULL, 0) == 0) {
        (void)wait_for_command_ack(uart_fd,
                                   LD2410B_CMD_DISABLE_CONFIG | 0x0100U);
    }
}

static int start_calibration(int uart_fd, struct rear_monitor *monitor,
                             unsigned int seconds)
{
    uint8_t duration[2];

    if (monitor->enabled || calibration_is_active(monitor)) {
        syslog(LOG_WARNING, "calibration rejected rear_enabled=%d state=%d",
               monitor->enabled, monitor->calibration);
        return -1;
    }

    memset(&monitor->parameters_before, 0,
           sizeof(monitor->parameters_before));
    memset(&monitor->parameters_after, 0,
           sizeof(monitor->parameters_after));
    monitor->parameters_changed = -1;
    monitor->snapshot_after_pending = 0;
    monitor->record_pending = 0;
    if (read_radar_parameters(uart_fd, &monitor->parameters_before) < 0) {
        syslog(LOG_ERR, "calibration rejected: parameter snapshot failed");
        return -1;
    }

    duration[0] = (uint8_t)(seconds & 0xffU);
    duration[1] = (uint8_t)(seconds >> 8);

    if (send_ld2410b_command(uart_fd, LD2410B_CMD_ENABLE_CONFIG,
                             (const uint8_t[]){ 0x01U, 0x00U }, 2U) < 0 ||
        wait_for_command_ack(uart_fd,
                             LD2410B_CMD_ENABLE_CONFIG | 0x0100U) < 0)
        return -1;

    if (send_ld2410b_command(uart_fd, LD2410B_CMD_START_CALIBRATION,
                             duration, sizeof(duration)) < 0 ||
        wait_for_command_ack(uart_fd,
                             LD2410B_CMD_START_CALIBRATION | 0x0100U) < 0) {
        end_configuration_best_effort(uart_fd);
        return -1;
    }

    if (send_ld2410b_command(uart_fd, LD2410B_CMD_DISABLE_CONFIG,
                             NULL, 0) < 0 ||
        wait_for_command_ack(uart_fd,
                             LD2410B_CMD_DISABLE_CONFIG | 0x0100U) < 0)
        return -1;

    monitor->calibration = CALIBRATION_WAITING;
    monitor->calibration_seconds = seconds;
    write_calibration_record(monitor);
    syslog(LOG_INFO,
           "LD2410B calibration accepted: leave area within 10 seconds, "
           "sampling_seconds=%u", seconds);
    return 0;
}

static const char *calibration_state_name(enum calibration_state state)
{
    switch (state) {
    case CALIBRATION_IDLE:
        return "idle";
    case CALIBRATION_WAITING:
        return "waiting";
    case CALIBRATION_RUNNING:
        return "running";
    case CALIBRATION_SUCCESS:
        return "success";
    case CALIBRATION_FAILED:
        return "failed";
    case CALIBRATION_UNKNOWN:
        return "unknown";
    default:
        return "unknown";
    }
}

static int process_ld2410b_frames(int event_fd, struct rear_monitor *monitor,
                                  uint8_t *buffer, size_t *buffer_length)
{
    size_t offset = 0;

    while (*buffer_length - offset >= 10U) {
        const uint8_t *frame = buffer + offset;
        const uint8_t *payload;
        size_t payload_length;
        size_t frame_length;
        uint8_t report_mode;
        uint8_t state;
        uint16_t moving_distance_cm;
        uint8_t moving_energy;
        uint16_t stationary_distance_cm;
        uint8_t stationary_energy;
        uint16_t detection_distance_cm;

        if (!header_matches(frame, ld2410b_data_header,
                            sizeof(ld2410b_data_header))) {
            ++offset;
            continue;
        }

        payload_length = (size_t)frame[4] | ((size_t)frame[5] << 8);
        frame_length = 4U + 2U + payload_length + 4U;

        if (frame_length > LD2410B_FRAME_BUFFER_SIZE) {
            ++offset;
            continue;
        }

        if (*buffer_length - offset < frame_length)
            break;

        if (!header_matches(frame + frame_length - 4U, ld2410b_data_footer,
                            sizeof(ld2410b_data_footer))) {
            ++offset;
            continue;
        }

        payload = frame + 6U;
        if (payload_length >= LD2410B_NORMAL_PAYLOAD_LENGTH &&
            (payload[0] == 0x01U || payload[0] == 0x02U) &&
            payload[1] == 0xaaU && payload[2] <= 0x06U &&
            payload[payload_length - 2U] == 0x55U &&
            payload[payload_length - 1U] == 0x00U) {
            report_mode = payload[0];
            state = payload[2];
            moving_distance_cm =
                (uint16_t)payload[3] | ((uint16_t)payload[4] << 8);
            moving_energy = payload[5];
            stationary_distance_cm =
                (uint16_t)payload[6] | ((uint16_t)payload[7] << 8);
            stationary_energy = payload[8];
            detection_distance_cm =
                (uint16_t)payload[9] | ((uint16_t)payload[10] << 8);

            if (clock_gettime(CLOCK_MONOTONIC,
                              &monitor->latest_radar_sample_time) < 0) {
                syslog(LOG_ERR, "clock_gettime failed: %s",
                       strerror(errno));
                return -1;
            }
            monitor->radar_sample_valid = 1;
            monitor->radar_sample_stale_reported = 0;
            monitor->latest_report_mode = report_mode;
            monitor->latest_radar_state = state;
            monitor->latest_moving_distance_cm = moving_distance_cm;
            monitor->latest_moving_energy = moving_energy;
            monitor->latest_stationary_distance_cm = stationary_distance_cm;
            monitor->latest_stationary_energy = stationary_energy;
            monitor->latest_detection_distance_cm = detection_distance_cm;
            monitor->latest_payload_length = payload_length;
            memcpy(monitor->latest_payload_prefix, payload,
                   sizeof(monitor->latest_payload_prefix));
            monitor->latest_gate_energy_valid = 0;

            if (report_mode == 0x01U && payload_length >= 17U &&
                payload[11] <= LD2410B_MAX_GATE &&
                payload[12] <= LD2410B_MAX_GATE) {
                size_t moving_gate_count = (size_t)payload[11] + 1U;
                size_t stationary_gate_count = (size_t)payload[12] + 1U;
                size_t stationary_gate_offset =
                    13U + moving_gate_count;
                size_t light_offset =
                    stationary_gate_offset + stationary_gate_count;
                size_t required_length = light_offset + 4U;

                if (payload_length >= required_length) {
                    memset(monitor->latest_moving_gate_energy, 0,
                           sizeof(monitor->latest_moving_gate_energy));
                    memset(monitor->latest_stationary_gate_energy, 0,
                           sizeof(monitor->latest_stationary_gate_energy));
                    memcpy(monitor->latest_moving_gate_energy,
                           payload + 13U, moving_gate_count);
                    memcpy(monitor->latest_stationary_gate_energy,
                           payload + stationary_gate_offset,
                           stationary_gate_count);
                    monitor->latest_moving_max_gate = payload[11];
                    monitor->latest_stationary_max_gate = payload[12];
                    monitor->latest_light_value = payload[light_offset];
                    monitor->latest_out_state = payload[light_offset + 1U];
                    monitor->latest_gate_energy_valid = 1;
                }
            }
            ++monitor->radar_frame_count;

            if (state == LD2410B_STATE_CALIBRATING) {
                monitor->calibration = CALIBRATION_RUNNING;
                syslog(LOG_INFO, "LD2410B calibration running");
            } else if (state == LD2410B_STATE_CALIBRATION_SUCCESS) {
                monitor->calibration = CALIBRATION_SUCCESS;
                monitor->snapshot_after_pending = 1;
                syslog(LOG_INFO, "LD2410B calibration success");
            } else if (state == LD2410B_STATE_CALIBRATION_FAILED) {
                monitor->calibration = CALIBRATION_FAILED;
                monitor->record_pending = 1;
                syslog(LOG_ERR, "LD2410B calibration failed");
            } else if (state <= 0x03U) {
                syslog(LOG_INFO,
                       "ld2410b mode=%u state=%u moving_cm=%u moving_energy=%u "
                       "stationary_cm=%u stationary_energy=%u detection_cm=%u",
                       report_mode, state, moving_distance_cm, moving_energy,
                       stationary_distance_cm, stationary_energy,
                       detection_distance_cm);

                if (update_rear_monitor(event_fd, monitor, state,
                                        detection_distance_cm) < 0)
                    return -1;
            }
        }

        offset += frame_length;
    }

    if (offset > 0) {
        memmove(buffer, buffer + offset, *buffer_length - offset);
        *buffer_length -= offset;
    }

    return 0;
}

static int read_ld2410b_uart(int uart_fd, int event_fd,
                             struct rear_monitor *monitor,
                             uint8_t *frame_buffer, size_t *frame_length)
{
    uint8_t read_buffer[LD2410B_READ_BUFFER_SIZE];

    for (;;) {
        ssize_t bytes;

        bytes = read(uart_fd, read_buffer, sizeof(read_buffer));
        if (bytes < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return 0;

            syslog(LOG_ERR, "read %s failed: %s",
                   LD2410B_DEVICE, strerror(errno));
            return -1;
        }

        if (bytes == 0)
            return 0;

        if ((size_t)bytes > LD2410B_FRAME_BUFFER_SIZE - *frame_length) {
            syslog(LOG_WARNING, "LD2410B receive buffer resynchronized");
            *frame_length = 0;
        }

        memcpy(frame_buffer + *frame_length, read_buffer, (size_t)bytes);
        *frame_length += (size_t)bytes;

        if (process_ld2410b_frames(event_fd, monitor,
                                   frame_buffer, frame_length) < 0)
            return -1;
    }
}

static int set_nonblocking(int fd)
{
    int flags;

    flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        syslog(LOG_ERR, "set nonblocking failed: %s", strerror(errno));
        return -1;
    }

    return 0;
}

static int create_control_socket(void)
{
    struct sockaddr_un address;
    int fd;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        syslog(LOG_ERR, "control socket failed: %s", strerror(errno));
        return -1;
    }

    if (set_nonblocking(fd) < 0) {
        close(fd);
        return -1;
    }

    if (unlink(CONTROL_SOCKET_PATH) < 0 && errno != ENOENT) {
        syslog(LOG_ERR, "unlink %s failed: %s",
               CONTROL_SOCKET_PATH, strerror(errno));
        close(fd);
        return -1;
    }

    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s",
             CONTROL_SOCKET_PATH);

    if (bind(fd, (const struct sockaddr *)&address, sizeof(address)) < 0 ||
        chmod(CONTROL_SOCKET_PATH, 0660) < 0 ||
        listen(fd, CONTROL_BACKLOG) < 0) {
        syslog(LOG_ERR, "create control socket %s failed: %s",
               CONTROL_SOCKET_PATH, strerror(errno));
        close(fd);
        unlink(CONTROL_SOCKET_PATH);
        return -1;
    }

    return fd;
}

static int parse_calibration_seconds(const char *request,
                                     unsigned int *seconds)
{
    static const char prefix[] = "rear calibrate ";
    const char *value;
    char *end;
    unsigned long parsed;

    if (strncmp(request, prefix, sizeof(prefix) - 1U) != 0)
        return -1;

    value = request + sizeof(prefix) - 1U;
    errno = 0;
    parsed = strtoul(value, &end, 10);

    if (errno != 0 || value == end ||
        (*end != '\n' && *end != '\0') || parsed == 0U ||
        parsed > 65535U)
        return -1;

    *seconds = (unsigned int)parsed;
    return 0;
}

static void expire_stale_radar_timers(struct rear_monitor *monitor)
{
    if (!monitor->radar_sample_valid || radar_sample_is_fresh(monitor))
        return;

    clear_rear_timers(monitor);
    if (!monitor->radar_sample_stale_reported) {
        monitor->radar_sample_stale_reported = 1;
        syslog(LOG_ERR,
               "LD2410B target sample stale; rear timers cleared, "
               "user_alarm unchanged");
    }
}

static int parse_nobody_seconds(const char *request, unsigned int *seconds)
{
    static const char prefix[] = "rear set-nobody ";
    const char *value;
    char *end;
    unsigned long parsed;

    if (strncmp(request, prefix, sizeof(prefix) - 1U) != 0)
        return -1;

    value = request + sizeof(prefix) - 1U;
    errno = 0;
    parsed = strtoul(value, &end, 10);

    if (errno != 0 || value == end ||
        (*end != '\n' && *end != '\0') ||
        parsed < LD2410B_MIN_NOBODY_SECONDS ||
        parsed > LD2410B_MAX_NOBODY_SECONDS)
        return -1;

    *seconds = (unsigned int)parsed;
    return 0;
}

static int parse_maximum_gate(const char *request, unsigned int *maximum_gate)
{
    static const char prefix[] = "rear set-range ";
    const char *value;
    char *end;
    unsigned long parsed;

    if (strncmp(request, prefix, sizeof(prefix) - 1U) != 0)
        return -1;

    value = request + sizeof(prefix) - 1U;
    errno = 0;
    parsed = strtoul(value, &end, 10);
    if (errno != 0 || value == end ||
        (*end != '\n' && *end != '\0') ||
        parsed < LD2410B_MIN_CONFIG_GATE || parsed > LD2410B_MAX_GATE)
        return -1;

    *maximum_gate = (unsigned int)parsed;
    return 0;
}

static int parse_resolution_mm(const char *request,
                               unsigned int *resolution_mm)
{
    static const char prefix[] = "rear set-resolution ";
    const char *value;
    char *end;
    unsigned long parsed;

    if (strncmp(request, prefix, sizeof(prefix) - 1U) != 0)
        return -1;

    value = request + sizeof(prefix) - 1U;
    errno = 0;
    parsed = strtoul(value, &end, 10);
    if (errno != 0 || value == end ||
        (*end != '\n' && *end != '\0') ||
        (parsed != LD2410B_RESOLUTION_750_MM &&
         parsed != LD2410B_RESOLUTION_200_MM))
        return -1;

    *resolution_mm = (unsigned int)parsed;
    return 0;
}

static void handle_control_clients(int control_fd, int event_fd, int uart_fd,
                                   struct rear_monitor *monitor)
{
    for (;;) {
        char request[CONTROL_REQUEST_SIZE];
        char response[CONTROL_RESPONSE_SIZE];
        ssize_t bytes;
        int client_fd;
        unsigned int maximum_gate;
        unsigned int resolution_mm;
        unsigned int seconds;

        client_fd = accept(control_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;

            syslog(LOG_ERR, "accept control client failed: %s",
                   strerror(errno));
            return;
        }

        bytes = read(client_fd, request, sizeof(request) - 1U);
        if (bytes <= 0) {
            if (bytes < 0)
                syslog(LOG_ERR, "read control request failed: %s",
                       strerror(errno));
            close(client_fd);
            continue;
        }

        request[bytes] = '\0';

        if (strcmp(request, "snapshot\n") == 0) {
            if (format_snapshot_response(monitor, response, sizeof(response)) < 0)
                snprintf(response, sizeof(response), "error snapshot format failed\n");
        } else if (strcmp(request, "ai on\n") == 0) {
            if (set_ai_user_alarm(event_fd, monitor, 1) == 0)
                snprintf(response, sizeof(response), "ai enabled\n");
            else
                snprintf(response, sizeof(response), "error ai on failed\n");
        } else if (strcmp(request, "ai off\n") == 0) {
            if (set_ai_user_alarm(event_fd, monitor, 0) == 0)
                snprintf(response, sizeof(response), "ai disabled\n");
            else
                snprintf(response, sizeof(response), "error ai off failed\n");
        } else if (strcmp(request, "rear enable\n") == 0) {
            if (enable_rear_monitor(event_fd, monitor) == 0)
                snprintf(response, sizeof(response), "rear enabled\n");
            else
                snprintf(response, sizeof(response),
                         "error rear enable rejected\n");
        } else if (strcmp(request, "rear disable\n") == 0) {
            if (disable_rear_monitor(event_fd, monitor) == 0)
                snprintf(response, sizeof(response), "rear disabled\n");
            else
                snprintf(response, sizeof(response),
                         "error rear disable\n");
        } else if (strcmp(request, "rear energy\n") == 0) {
            uint64_t sample_age_ms;

            if (!monitor->radar_sample_valid ||
                radar_sample_age_ms(monitor, &sample_age_ms) < 0) {
                snprintf(response, sizeof(response),
                         "error rear energy unavailable\n");
            } else {
                snprintf(response, sizeof(response),
                         "rear energy f=%" PRIu64 " age=%" PRIu64
                         " mode=%s state=%u move=%u/%u static=%u/%u "
                         "detect=%u\n",
                         monitor->radar_frame_count, sample_age_ms,
                         monitor->latest_report_mode == 0x01U ?
                         "engineering" : "basic",
                         (unsigned int)monitor->latest_radar_state,
                         (unsigned int)monitor->latest_moving_distance_cm,
                         (unsigned int)monitor->latest_moving_energy,
                         (unsigned int)monitor->latest_stationary_distance_cm,
                         (unsigned int)monitor->latest_stationary_energy,
                         (unsigned int)monitor->latest_detection_distance_cm);
            }
        } else if (strcmp(request, "rear gates\n") == 0) {
            char moving_energy[64];
            char stationary_energy[64];
            uint64_t sample_age_ms;
            size_t moving_gate_count;
            size_t stationary_gate_count;

            moving_gate_count =
                (size_t)monitor->latest_moving_max_gate + 1U;
            stationary_gate_count =
                (size_t)monitor->latest_stationary_max_gate + 1U;

            if (!monitor->radar_sample_valid ||
                !monitor->latest_gate_energy_valid ||
                radar_sample_age_ms(monitor, &sample_age_ms) < 0) {
                snprintf(response, sizeof(response),
                         "error rear gates unavailable; "
                         "engineering frame required\n");
            } else if (format_u8_list(
                           monitor->latest_moving_gate_energy,
                           moving_gate_count, moving_energy,
                           sizeof(moving_energy)) < 0 ||
                       format_u8_list(
                           monitor->latest_stationary_gate_energy,
                           stationary_gate_count, stationary_energy,
                           sizeof(stationary_energy)) < 0) {
                snprintf(response, sizeof(response),
                         "error rear gates format failed\n");
            } else {
                snprintf(response, sizeof(response),
                         "rear gates f=%" PRIu64 " age=%" PRIu64
                         " moving_max=%u stationary_max=%u "
                         "moving=%s stationary=%s light=%u out=%u\n",
                         monitor->radar_frame_count, sample_age_ms,
                         (unsigned int)monitor->latest_moving_max_gate,
                         (unsigned int)monitor->latest_stationary_max_gate,
                         moving_energy, stationary_energy,
                         (unsigned int)monitor->latest_light_value,
                         (unsigned int)monitor->latest_out_state);
            }
        } else if (strcmp(request, "rear raw\n") == 0) {
            char payload_hex[2U * LD2410B_DIAGNOSTIC_PREFIX_LENGTH + 1U];
            char sample_age_text[32];
            uint64_t sample_age_ms;

            if (!monitor->radar_sample_valid) {
                snprintf(response, sizeof(response),
                         "rear raw frame=0 age_ms=unknown mode=unknown "
                         "payload_len=0 prefix=none\n");
            } else {
                if (radar_sample_age_ms(monitor, &sample_age_ms) == 0)
                    snprintf(sample_age_text, sizeof(sample_age_text),
                             "%" PRIu64, sample_age_ms);
                else
                    snprintf(sample_age_text, sizeof(sample_age_text),
                             "unknown");

                if (format_hex_bytes(monitor->latest_payload_prefix,
                                     sizeof(monitor->latest_payload_prefix),
                                     payload_hex, sizeof(payload_hex)) < 0)
                    snprintf(payload_hex, sizeof(payload_hex), "error");

                snprintf(response, sizeof(response),
                         "rear raw frame=%" PRIu64 " age_ms=%s mode=%s "
                         "payload_len=%zu prefix=%s\n",
                         monitor->radar_frame_count, sample_age_text,
                         monitor->latest_report_mode == 0x01U ?
                         "engineering" : "basic",
                         monitor->latest_payload_length, payload_hex);
            }
        } else if (strcmp(request, "rear status\n") == 0) {
            unsigned int module_state = 0;
            const char *module_state_text = "not-queried";
            const char *report_mode_text = "unknown";
            char frame_count_text[32];
            char sample_age_text[32];
            char radar_state_text[16];
            char detection_distance_text[16];
            char moving_distance_text[16];
            char moving_energy_text[16];
            char stationary_distance_text[16];
            char stationary_energy_text[16];
            uint64_t sample_age_ms;

            if (monitor->radar_sample_valid) {
                snprintf(frame_count_text, sizeof(frame_count_text), "%" PRIu64,
                         monitor->radar_frame_count);
                if (radar_sample_age_ms(monitor, &sample_age_ms) == 0)
                    snprintf(sample_age_text, sizeof(sample_age_text),
                             "%" PRIu64, sample_age_ms);
                else
                    snprintf(sample_age_text, sizeof(sample_age_text),
                             "unknown");
                snprintf(radar_state_text, sizeof(radar_state_text), "%u",
                         monitor->latest_radar_state);
                snprintf(detection_distance_text,
                         sizeof(detection_distance_text), "%u",
                         monitor->latest_radar_state == LD2410B_STATE_NONE ?
                         0U : monitor->latest_detection_distance_cm);
                snprintf(moving_distance_text, sizeof(moving_distance_text),
                         "%u",
                         monitor->latest_radar_state == LD2410B_STATE_NONE ?
                         0U : monitor->latest_moving_distance_cm);
                snprintf(moving_energy_text, sizeof(moving_energy_text), "%u",
                         monitor->latest_radar_state == LD2410B_STATE_NONE ?
                         0U : monitor->latest_moving_energy);
                snprintf(stationary_distance_text,
                         sizeof(stationary_distance_text), "%u",
                         monitor->latest_radar_state == LD2410B_STATE_NONE ?
                         0U : monitor->latest_stationary_distance_cm);
                snprintf(stationary_energy_text,
                         sizeof(stationary_energy_text), "%u",
                         monitor->latest_radar_state == LD2410B_STATE_NONE ?
                         0U : monitor->latest_stationary_energy);
                report_mode_text = monitor->latest_report_mode == 0x01U ?
                                   "engineering" : "basic";
            } else {
                snprintf(frame_count_text, sizeof(frame_count_text), "0");
                snprintf(sample_age_text, sizeof(sample_age_text), "unknown");
                snprintf(radar_state_text, sizeof(radar_state_text),
                         "unknown");
                snprintf(detection_distance_text,
                         sizeof(detection_distance_text), "unknown");
                snprintf(moving_distance_text, sizeof(moving_distance_text),
                         "unknown");
                snprintf(moving_energy_text, sizeof(moving_energy_text),
                         "unknown");
                snprintf(stationary_distance_text,
                         sizeof(stationary_distance_text), "unknown");
                snprintf(stationary_energy_text,
                         sizeof(stationary_energy_text), "unknown");
            }

            if (!monitor->enabled) {
                if (query_calibration_state(uart_fd, &module_state) == 0) {
                    module_state_text =
                        module_calibration_state_name(module_state);

                    if (module_state == 1U)
                        monitor->calibration = CALIBRATION_RUNNING;
                    else if (module_state == 2U) {
                        monitor->calibration = CALIBRATION_SUCCESS;
                        monitor->snapshot_after_pending = 1;
                    }
                } else {
                    module_state_text = "query-failed";
                }
            }

            finish_calibration_if_needed(uart_fd, monitor);

            snprintf(response, sizeof(response),
                     "rear %s user_alarm=%s radar_alarm=%s ai_alarm=%s "
                     "frame=%s age_ms=%s mode=%s "
                     "radar_state=%s detect_cm=%s move_cm=%s move_energy=%s "
                     "static_cm=%s static_energy=%s calibration=%s "
                     "module=%s parameters=%s\n",
                     monitor->enabled ? "enabled" : "disabled",
                     monitor->applied_user_alarm_on ? "on" : "off",
                     monitor->user_alarm_on ? "on" : "off",
                     monitor->ai_alarm_on ? "on" : "off",
                     frame_count_text, sample_age_text, report_mode_text,
                     radar_state_text, detection_distance_text,
                     moving_distance_text, moving_energy_text,
                     stationary_distance_text, stationary_energy_text,
                     calibration_state_name(monitor->calibration),
                     module_state_text,
                     monitor->parameters_changed < 0 ? "unknown" :
                     (monitor->parameters_changed ? "changed" :
                                                    "unchanged"));
        } else if (strcmp(request, "rear config\n") == 0) {
            struct radar_parameters parameters;

            memset(&parameters, 0, sizeof(parameters));
            if (monitor->enabled || calibration_is_active(monitor)) {
                snprintf(response, sizeof(response),
                         "error rear config requires disabled idle state\n");
            } else if (read_radar_resolution(uart_fd, &resolution_mm) == 0 &&
                       read_radar_parameters(uart_fd, &parameters) == 0 &&
                       parameters.valid) {
                snprintf(response, sizeof(response),
                         "rear config resolution_mm=%u moving_max=%u "
                         "stationary_max=%u nobody_seconds=%u\n",
                         resolution_mm,
                         (unsigned int)parameters.moving_maximum_gate,
                         (unsigned int)parameters.stationary_maximum_gate,
                         (unsigned int)parameters.nobody_seconds);
            } else {
                snprintf(response, sizeof(response),
                         "error rear config query failed\n");
            }
        } else if (strcmp(request, "rear resolution\n") == 0) {
            if (monitor->enabled || calibration_is_active(monitor)) {
                snprintf(response, sizeof(response),
                         "error rear resolution requires disabled idle state\n");
            } else if (read_radar_resolution(uart_fd, &resolution_mm) == 0) {
                snprintf(response, sizeof(response),
                         "rear resolution_mm=%u verified\n", resolution_mm);
            } else {
                snprintf(response, sizeof(response),
                         "error rear resolution query failed\n");
            }
        } else if (parse_resolution_mm(request, &resolution_mm) == 0) {
            int restarted = 0;

            if (set_radar_resolution(uart_fd, monitor, resolution_mm,
                                     &restarted) == 0) {
                snprintf(response, sizeof(response),
                         "rear resolution_mm=%u verified restarted=%s\n",
                         resolution_mm, restarted ? "yes" : "no");
            } else {
                snprintf(response, sizeof(response),
                         "error rear set-resolution rejected or failed\n");
            }
        } else if (parse_maximum_gate(request, &maximum_gate) == 0) {
            if (set_radar_maximum_gate(uart_fd, monitor,
                                       maximum_gate) == 0) {
                snprintf(response, sizeof(response),
                         "rear moving_max=%u stationary_max=%u verified\n",
                         maximum_gate, maximum_gate);
            } else {
                snprintf(response, sizeof(response),
                         "error rear set-range rejected or failed\n");
            }
        } else if (parse_nobody_seconds(request, &seconds) == 0) {
            if (set_radar_nobody_seconds(uart_fd, monitor, seconds) == 0) {
                snprintf(response, sizeof(response),
                         "rear nobody_seconds=%u verified\n", seconds);
            } else {
                snprintf(response, sizeof(response),
                         "error rear set-nobody rejected or failed\n");
            }
        } else if (parse_calibration_seconds(request, &seconds) == 0) {
            if (start_calibration(uart_fd, monitor, seconds) == 0) {
                snprintf(response, sizeof(response),
                         "rear calibration waiting seconds=%u\n", seconds);
            } else {
                snprintf(response, sizeof(response),
                         "error rear calibration rejected or failed\n");
            }
        } else {
            snprintf(response, sizeof(response), "error invalid command\n");
        }

        if (send(client_fd, response, strlen(response), MSG_NOSIGNAL) < 0)
            syslog(LOG_ERR, "control response failed: %s", strerror(errno));

        close(client_fd);
    }
}

static int handle_event(const struct safety_event_record *event,
                        struct rear_monitor *monitor)
{
    if (event->source == SAFETY_EVENT_SOURCE_BH1750 &&
        (event->type == SAFETY_EVENT_TYPE_BH1750_SAMPLE ||
         event->type == SAFETY_EVENT_TYPE_LOW_LUX ||
         event->type == SAFETY_EVENT_TYPE_LUX_RECOVER)) {
        struct timespec now;

        if (clock_gettime(CLOCK_MONOTONIC, &now) == 0) {
            monitor->latest_bh1750_lux = event->lux;
            monitor->latest_bh1750_sample_time = now;
            monitor->bh1750_sample_valid = 1;
        }
    }
    if (event->source == SAFETY_EVENT_SOURCE_DOOR_GPIO &&
        event->type == SAFETY_EVENT_TYPE_DOOR_STATE_CHANGED &&
        (event->state == SAFETY_EVENT_STATE_LOGICAL_ACTIVE ||
         event->state == SAFETY_EVENT_STATE_LOGICAL_INACTIVE)) {
        struct timespec now;

        if (clock_gettime(CLOCK_MONOTONIC, &now) == 0) {
            monitor->door_sample_valid = 1;
            monitor->latest_door_sequence = event->sequence;
            monitor->latest_door_state = event->state;
            monitor->latest_door_event_timestamp_ns = event->timestamp_ns;
            monitor->latest_door_sample_time = now;
        }
    }
    syslog(LOG_INFO,
           "sequence=%u type=%u source=%u state=%u lux=%u timestamp_ns=%" PRIu64,
           event->sequence, event->type, event->source, event->state,
           event->lux, (uint64_t)event->timestamp_ns);
    return 0;
}

static void sample_sht3x_if_due(struct timespec *last_sample)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
        syslog(LOG_ERR, "clock_gettime failed: %s", strerror(errno));
        return;
    }

    if (elapsed_at_least(last_sample, &now,
                         SHT3X_SAMPLE_INTERVAL_MS / 1000)) {
        sample_sht3x();
        *last_sample = now;
    }
}

static int run_event_loop(int event_fd, int uart_fd, int control_fd,
                          struct rear_monitor *monitor)
{
    uint8_t frame_buffer[LD2410B_FRAME_BUFFER_SIZE];
    size_t frame_length = 0;
    struct timespec last_sht3x_sample;
    struct pollfd pfds[3];

    memset(frame_buffer, 0, sizeof(frame_buffer));
    memset(pfds, 0, sizeof(pfds));

    if (clock_gettime(CLOCK_MONOTONIC, &last_sht3x_sample) < 0) {
        syslog(LOG_ERR, "clock_gettime failed: %s", strerror(errno));
        return -1;
    }

    pfds[0].fd = event_fd;
    pfds[0].events = POLLIN;
    pfds[1].fd = uart_fd;
    pfds[1].events = POLLIN;
    pfds[2].fd = control_fd;
    pfds[2].events = POLLIN;

    while (!stop_requested) {
        int poll_rc;

        poll_rc = poll(pfds, 3, SHT3X_SAMPLE_INTERVAL_MS);
        if (poll_rc < 0) {
            if (errno == EINTR)
                continue;

            syslog(LOG_ERR, "poll failed: %s", strerror(errno));
            return -1;
        }

        if (pfds[0].revents & POLLIN) {
            struct safety_event_record event;
            ssize_t bytes;

            bytes = read(event_fd, &event, sizeof(event));
            if (bytes < 0) {
                syslog(LOG_ERR, "read safety event failed: %s",
                       strerror(errno));
                return -1;
            }

            if (bytes != (ssize_t)sizeof(event)) {
                syslog(LOG_ERR, "read length %zd, expected %zu",
                       bytes, sizeof(event));
                return -1;
            }

            if (handle_event(&event, monitor) < 0)
                return -1;
        }

        if (pfds[1].revents & POLLIN) {
            if (read_ld2410b_uart(uart_fd, event_fd, monitor,
                                  frame_buffer, &frame_length) < 0)
                return -1;
            finish_calibration_if_needed(uart_fd, monitor);
        }

        if (pfds[2].revents & POLLIN)
            handle_control_clients(control_fd, event_fd, uart_fd, monitor);

        if ((pfds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) ||
            (pfds[1].revents & (POLLERR | POLLHUP | POLLNVAL)) ||
            (pfds[2].revents & (POLLERR | POLLHUP | POLLNVAL))) {
            syslog(LOG_ERR, "poll error event=0x%x uart=0x%x control=0x%x",
                   pfds[0].revents, pfds[1].revents, pfds[2].revents);
            return -1;
        }

        expire_stale_radar_timers(monitor);
        sample_sht3x_if_due(&last_sht3x_sample);
    }

    return 0;
}

static int install_signal_handlers(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = request_stop;
    sigemptyset(&action.sa_mask);

    if (sigaction(SIGINT, &action, NULL) < 0 ||
        sigaction(SIGTERM, &action, NULL) < 0) {
        syslog(LOG_ERR, "sigaction failed: %s", strerror(errno));
        return -1;
    }

    return 0;
}

int main(void)
{
    struct rear_monitor monitor;
    int control_fd = -1;
    int event_fd = -1;
    int uart_fd = -1;
    int rc = EXIT_FAILURE;

    memset(&monitor, 0, sizeof(monitor));
    monitor.user_alarm_on = 0;
    monitor.ai_alarm_on = 0;
    monitor.applied_user_alarm_on = -1;
    monitor.parameters_changed = -1;
    load_calibration_record(&monitor);

    openlog("safety_alarmd", LOG_PID, LOG_DAEMON);

    if (install_signal_handlers() < 0)
        goto out;

    event_fd = open(SAFETY_EVENT_DEVICE, O_RDONLY);
    if (event_fd < 0) {
        syslog(LOG_ERR, "open %s failed: %s",
               SAFETY_EVENT_DEVICE, strerror(errno));
        goto out;
    }

    if (check_api_version(event_fd) < 0)
        goto out;

    if (set_radar_user_alarm(event_fd, &monitor, 0) < 0)
        goto out;

    uart_fd = open_ld2410b_uart();
    if (uart_fd < 0)
        goto out;

    control_fd = create_control_socket();
    if (control_fd < 0)
        goto out;

    syslog(LOG_INFO,
           "started rear monitor default=disabled calibration=manual-only");

    if (run_event_loop(event_fd, uart_fd, control_fd, &monitor) == 0)
        rc = EXIT_SUCCESS;

out:
    if (event_fd >= 0 &&
        clear_all_user_alarms(event_fd, &monitor) < 0) {
        syslog(LOG_ERR, "failed to clear user alarms during exit");
    }

    if (control_fd >= 0)
        close(control_fd);
    unlink(CONTROL_SOCKET_PATH);

    if (uart_fd >= 0)
        close(uart_fd);
    if (event_fd >= 0)
        close(event_fd);

    closelog();
    return rc;
}

static int wait_for_parameters_ack(int uart_fd,
                                   struct radar_parameters *parameters)
{
    uint8_t buffer[LD2410B_CONFIG_BUFFER_SIZE];
    size_t buffer_length = 0;

    for (;;) {
        struct pollfd pfd = { .fd = uart_fd, .events = POLLIN };
        size_t offset = 0;
        ssize_t bytes;
        int poll_rc;

        poll_rc = poll(&pfd, 1, LD2410B_CONFIG_TIMEOUT_MS);
        if (poll_rc <= 0) {
            syslog(LOG_ERR, "LD2410B parameter response timed out");
            return -1;
        }

        bytes = read(uart_fd, buffer + buffer_length,
                     sizeof(buffer) - buffer_length);
        if (bytes < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            syslog(LOG_ERR, "read LD2410B parameters failed: %s",
                   strerror(errno));
            return -1;
        }
        if (bytes == 0)
            continue;

        buffer_length += (size_t)bytes;
        while (buffer_length - offset >= 10U) {
            const uint8_t *frame = buffer + offset;
            const uint8_t *payload;
            size_t payload_length;
            size_t frame_length;
            size_t gate_count;
            size_t required_length;
            size_t index;
            uint16_t ack_command;
            uint16_t ack_status;

            if (!header_matches(frame, ld2410b_command_header,
                                sizeof(ld2410b_command_header))) {
                ++offset;
                continue;
            }

            payload_length = (size_t)frame[4] | ((size_t)frame[5] << 8);
            frame_length = 6U + payload_length + 4U;
            if (frame_length > sizeof(buffer)) {
                ++offset;
                continue;
            }
            if (buffer_length - offset < frame_length)
                break;
            if (!header_matches(frame + frame_length - 4U,
                                ld2410b_command_footer,
                                sizeof(ld2410b_command_footer))) {
                ++offset;
                continue;
            }

            payload = frame + 6U;
            if (payload_length < 10U) {
                offset += frame_length;
                continue;
            }

            ack_command = (uint16_t)payload[0] |
                          ((uint16_t)payload[1] << 8);
            ack_status = (uint16_t)payload[2] |
                         ((uint16_t)payload[3] << 8);
            if (ack_command !=
                (LD2410B_CMD_READ_PARAMETERS | 0x0100U)) {
                offset += frame_length;
                continue;
            }
            if (ack_status != 0U || payload[4] != 0xaaU ||
                payload[5] > LD2410B_MAX_GATE) {
                syslog(LOG_ERR, "invalid LD2410B parameter response");
                return -1;
            }

            gate_count = (size_t)payload[5] + 1U;
            required_length = 10U + 2U * gate_count;
            if (payload_length < required_length) {
                syslog(LOG_ERR, "short LD2410B parameter response");
                return -1;
            }

            memset(parameters, 0, sizeof(*parameters));
            parameters->maximum_gate = payload[5];
            parameters->moving_maximum_gate = payload[6];
            parameters->stationary_maximum_gate = payload[7];
            for (index = 0; index < gate_count; ++index) {
                parameters->moving_sensitivity[index] = payload[8U + index];
                parameters->stationary_sensitivity[index] =
                    payload[8U + gate_count + index];
            }
            parameters->nobody_seconds =
                (uint16_t)payload[8U + 2U * gate_count] |
                ((uint16_t)payload[9U + 2U * gate_count] << 8);
            parameters->valid = 1;
            return 0;
        }

        if (offset > 0) {
            memmove(buffer, buffer + offset, buffer_length - offset);
            buffer_length -= offset;
        }
        if (buffer_length == sizeof(buffer))
            buffer_length = 0;
    }
}

static int read_radar_parameters(int uart_fd,
                                 struct radar_parameters *parameters)
{
    if (send_ld2410b_command(uart_fd, LD2410B_CMD_ENABLE_CONFIG,
                             (const uint8_t[]){ 0x01U, 0x00U }, 2U) < 0 ||
        wait_for_command_ack(uart_fd,
                             LD2410B_CMD_ENABLE_CONFIG | 0x0100U) < 0)
        return -1;

    if (send_ld2410b_command(uart_fd, LD2410B_CMD_READ_PARAMETERS,
                             NULL, 0) < 0 ||
        wait_for_parameters_ack(uart_fd, parameters) < 0) {
        end_configuration_best_effort(uart_fd);
        return -1;
    }

    if (send_ld2410b_command(uart_fd, LD2410B_CMD_DISABLE_CONFIG,
                             NULL, 0) < 0 ||
        wait_for_command_ack(uart_fd,
                             LD2410B_CMD_DISABLE_CONFIG | 0x0100U) < 0)
        return -1;

    return 0;
}

static void encode_u32_le(uint8_t *destination, uint32_t value)
{
    destination[0] = (uint8_t)(value & 0xffU);
    destination[1] = (uint8_t)((value >> 8) & 0xffU);
    destination[2] = (uint8_t)((value >> 16) & 0xffU);
    destination[3] = (uint8_t)((value >> 24) & 0xffU);
}

static int resolution_index_to_mm(uint16_t index,
                                  unsigned int *resolution_mm)
{
    if (index == LD2410B_RESOLUTION_750_INDEX) {
        *resolution_mm = LD2410B_RESOLUTION_750_MM;
        return 0;
    }
    if (index == LD2410B_RESOLUTION_200_INDEX) {
        *resolution_mm = LD2410B_RESOLUTION_200_MM;
        return 0;
    }
    return -1;
}

static int resolution_mm_to_index(unsigned int resolution_mm,
                                  uint16_t *index)
{
    if (resolution_mm == LD2410B_RESOLUTION_750_MM) {
        *index = LD2410B_RESOLUTION_750_INDEX;
        return 0;
    }
    if (resolution_mm == LD2410B_RESOLUTION_200_MM) {
        *index = LD2410B_RESOLUTION_200_INDEX;
        return 0;
    }
    return -1;
}

static int query_radar_resolution_in_config(int uart_fd,
                                             unsigned int *resolution_mm)
{
    uint16_t index;

    if (send_ld2410b_command(uart_fd, LD2410B_CMD_QUERY_RESOLUTION,
                             NULL, 0) < 0 ||
        wait_for_u16_value_ack(
            uart_fd, LD2410B_CMD_QUERY_RESOLUTION | 0x0100U, &index) < 0 ||
        resolution_index_to_mm(index, resolution_mm) < 0) {
        syslog(LOG_ERR, "invalid LD2410B resolution response");
        return -1;
    }

    return 0;
}

static int read_radar_resolution(int uart_fd, unsigned int *resolution_mm)
{
    if (send_ld2410b_command(uart_fd, LD2410B_CMD_ENABLE_CONFIG,
                             (const uint8_t[]){ 0x01U, 0x00U }, 2U) < 0 ||
        wait_for_command_ack(uart_fd,
                             LD2410B_CMD_ENABLE_CONFIG | 0x0100U) < 0)
        return -1;

    if (query_radar_resolution_in_config(uart_fd, resolution_mm) < 0) {
        end_configuration_best_effort(uart_fd);
        return -1;
    }

    if (send_ld2410b_command(uart_fd, LD2410B_CMD_DISABLE_CONFIG,
                             NULL, 0) < 0 ||
        wait_for_command_ack(uart_fd,
                             LD2410B_CMD_DISABLE_CONFIG | 0x0100U) < 0)
        return -1;

    return 0;
}

static int wait_milliseconds(unsigned int milliseconds)
{
    struct timespec remaining = {
        .tv_sec = (time_t)(milliseconds / 1000U),
        .tv_nsec = (long)(milliseconds % 1000U) * 1000000L,
    };

    while (nanosleep(&remaining, &remaining) < 0) {
        if (errno != EINTR || stop_requested)
            return -1;
    }
    return 0;
}

static int set_radar_resolution(int uart_fd, struct rear_monitor *monitor,
                                unsigned int resolution_mm,
                                int *restarted)
{
    unsigned int before_mm;
    unsigned int stored_mm;
    unsigned int after_mm;
    uint16_t index;
    uint8_t value[2];

    *restarted = 0;
    if (monitor->enabled || calibration_is_active(monitor) ||
        resolution_mm_to_index(resolution_mm, &index) < 0) {
        syslog(LOG_WARNING,
               "set resolution rejected rear_enabled=%d calibration=%d "
               "resolution_mm=%u",
               monitor->enabled, monitor->calibration, resolution_mm);
        return -1;
    }

    if (read_radar_resolution(uart_fd, &before_mm) < 0)
        return -1;
    if (before_mm == resolution_mm) {
        syslog(LOG_INFO, "LD2410B resolution already %u mm", before_mm);
        return 0;
    }

    value[0] = (uint8_t)(index & 0xffU);
    value[1] = (uint8_t)(index >> 8);
    if (send_ld2410b_command(uart_fd, LD2410B_CMD_ENABLE_CONFIG,
                             (const uint8_t[]){ 0x01U, 0x00U }, 2U) < 0 ||
        wait_for_command_ack(uart_fd,
                             LD2410B_CMD_ENABLE_CONFIG | 0x0100U) < 0)
        return -1;

    if (send_ld2410b_command(uart_fd, LD2410B_CMD_SET_RESOLUTION,
                             value, sizeof(value)) < 0 ||
        wait_for_command_ack(uart_fd,
                             LD2410B_CMD_SET_RESOLUTION | 0x0100U) < 0 ||
        query_radar_resolution_in_config(uart_fd, &stored_mm) < 0 ||
        stored_mm != resolution_mm) {
        syslog(LOG_ERR,
               "LD2410B stored resolution verification failed requested=%u",
               resolution_mm);
        end_configuration_best_effort(uart_fd);
        return -1;
    }

    if (send_ld2410b_command(uart_fd, LD2410B_CMD_RESTART, NULL, 0) < 0 ||
        wait_for_command_ack(uart_fd,
                             LD2410B_CMD_RESTART | 0x0100U) < 0)
        return -1;

    *restarted = 1;
    clear_rear_timers(monitor);
    monitor->radar_sample_valid = 0;
    monitor->latest_gate_energy_valid = 0;
    if (wait_milliseconds(LD2410B_RESTART_SETTLE_MS) < 0 ||
        read_radar_resolution(uart_fd, &after_mm) < 0 ||
        after_mm != resolution_mm) {
        syslog(LOG_ERR,
               "LD2410B post-restart resolution verification failed "
               "requested=%u",
               resolution_mm);
        return -1;
    }

    syslog(LOG_INFO,
           "LD2410B resolution updated old=%u new=%u restarted and verified",
           before_mm, after_mm);
    return 0;
}

static int set_radar_maximum_gate(int uart_fd,
                                  struct rear_monitor *monitor,
                                  unsigned int maximum_gate)
{
    struct radar_parameters before;
    struct radar_parameters after;
    uint8_t value[18] = { 0 };

    if (monitor->enabled || calibration_is_active(monitor) ||
        maximum_gate < LD2410B_MIN_CONFIG_GATE ||
        maximum_gate > LD2410B_MAX_GATE) {
        syslog(LOG_WARNING,
               "set range rejected rear_enabled=%d calibration=%d gate=%u",
               monitor->enabled, monitor->calibration, maximum_gate);
        return -1;
    }

    memset(&before, 0, sizeof(before));
    memset(&after, 0, sizeof(after));
    if (read_radar_parameters(uart_fd, &before) < 0 || !before.valid) {
        syslog(LOG_ERR, "cannot read LD2410B parameters before range update");
        return -1;
    }
    if (before.moving_maximum_gate == maximum_gate &&
        before.stationary_maximum_gate == maximum_gate) {
        syslog(LOG_INFO, "LD2410B maximum gate already %u", maximum_gate);
        return 0;
    }

    value[0] = 0x00U;
    value[1] = 0x00U;
    encode_u32_le(value + 2, maximum_gate);
    value[6] = 0x01U;
    value[7] = 0x00U;
    encode_u32_le(value + 8, maximum_gate);
    value[12] = 0x02U;
    value[13] = 0x00U;
    encode_u32_le(value + 14, before.nobody_seconds);

    if (send_ld2410b_command(uart_fd, LD2410B_CMD_ENABLE_CONFIG,
                             (const uint8_t[]){ 0x01U, 0x00U }, 2U) < 0 ||
        wait_for_command_ack(uart_fd,
                             LD2410B_CMD_ENABLE_CONFIG | 0x0100U) < 0)
        return -1;

    if (send_ld2410b_command(uart_fd, LD2410B_CMD_SET_PARAMETERS,
                             value, sizeof(value)) < 0 ||
        wait_for_command_ack(uart_fd,
                             LD2410B_CMD_SET_PARAMETERS | 0x0100U) < 0) {
        end_configuration_best_effort(uart_fd);
        return -1;
    }

    if (send_ld2410b_command(uart_fd, LD2410B_CMD_DISABLE_CONFIG,
                             NULL, 0) < 0 ||
        wait_for_command_ack(uart_fd,
                             LD2410B_CMD_DISABLE_CONFIG | 0x0100U) < 0)
        return -1;

    if (read_radar_parameters(uart_fd, &after) < 0 || !after.valid ||
        after.moving_maximum_gate != maximum_gate ||
        after.stationary_maximum_gate != maximum_gate ||
        after.maximum_gate != before.maximum_gate ||
        after.nobody_seconds != before.nobody_seconds ||
        memcmp(after.moving_sensitivity, before.moving_sensitivity,
               sizeof(after.moving_sensitivity)) != 0 ||
        memcmp(after.stationary_sensitivity, before.stationary_sensitivity,
               sizeof(after.stationary_sensitivity)) != 0) {
        syslog(LOG_ERR,
               "LD2410B range update verification failed requested=%u",
               maximum_gate);
        return -1;
    }

    syslog(LOG_INFO,
           "LD2410B maximum gate updated moving=%u->%u stationary=%u->%u "
           "nobody=%u preserved and verified",
           before.moving_maximum_gate, after.moving_maximum_gate,
           before.stationary_maximum_gate,
           after.stationary_maximum_gate, after.nobody_seconds);
    return 0;
}

static int set_radar_nobody_seconds(int uart_fd,
                                    struct rear_monitor *monitor,
                                    unsigned int seconds)
{
    struct radar_parameters before;
    struct radar_parameters after;
    uint8_t value[18] = { 0 };

    if (monitor->enabled || calibration_is_active(monitor) ||
        seconds < LD2410B_MIN_NOBODY_SECONDS ||
        seconds > LD2410B_MAX_NOBODY_SECONDS) {
        syslog(LOG_WARNING,
               "set nobody rejected rear_enabled=%d calibration=%d seconds=%u",
               monitor->enabled, monitor->calibration, seconds);
        return -1;
    }

    memset(&before, 0, sizeof(before));
    memset(&after, 0, sizeof(after));
    if (read_radar_parameters(uart_fd, &before) < 0 ||
        !before.valid ||
        before.moving_maximum_gate < LD2410B_MIN_CONFIG_GATE ||
        before.moving_maximum_gate > LD2410B_MAX_GATE ||
        before.stationary_maximum_gate < LD2410B_MIN_CONFIG_GATE ||
        before.stationary_maximum_gate > LD2410B_MAX_GATE) {
        syslog(LOG_ERR, "cannot read valid LD2410B parameters before update");
        return -1;
    }

    value[0] = 0x00U;
    value[1] = 0x00U;
    encode_u32_le(value + 2, before.moving_maximum_gate);
    value[6] = 0x01U;
    value[7] = 0x00U;
    encode_u32_le(value + 8, before.stationary_maximum_gate);
    value[12] = 0x02U;
    value[13] = 0x00U;
    encode_u32_le(value + 14, seconds);

    if (send_ld2410b_command(uart_fd, LD2410B_CMD_ENABLE_CONFIG,
                             (const uint8_t[]){ 0x01U, 0x00U }, 2U) < 0 ||
        wait_for_command_ack(uart_fd,
                             LD2410B_CMD_ENABLE_CONFIG | 0x0100U) < 0)
        return -1;

    if (send_ld2410b_command(uart_fd, LD2410B_CMD_SET_PARAMETERS,
                             value, sizeof(value)) < 0 ||
        wait_for_command_ack(uart_fd,
                             LD2410B_CMD_SET_PARAMETERS | 0x0100U) < 0) {
        end_configuration_best_effort(uart_fd);
        return -1;
    }

    if (send_ld2410b_command(uart_fd, LD2410B_CMD_DISABLE_CONFIG,
                             NULL, 0) < 0 ||
        wait_for_command_ack(uart_fd,
                             LD2410B_CMD_DISABLE_CONFIG | 0x0100U) < 0)
        return -1;

    if (read_radar_parameters(uart_fd, &after) < 0 || !after.valid ||
        after.nobody_seconds != seconds ||
        after.maximum_gate != before.maximum_gate ||
        after.moving_maximum_gate != before.moving_maximum_gate ||
        after.stationary_maximum_gate != before.stationary_maximum_gate ||
        memcmp(after.moving_sensitivity, before.moving_sensitivity,
               sizeof(after.moving_sensitivity)) != 0 ||
        memcmp(after.stationary_sensitivity, before.stationary_sensitivity,
               sizeof(after.stationary_sensitivity)) != 0) {
        syslog(LOG_ERR,
               "LD2410B nobody update verification failed requested=%u",
               seconds);
        return -1;
    }

    syslog(LOG_INFO,
           "LD2410B nobody_seconds updated old=%u new=%u and verified",
           before.nobody_seconds, after.nobody_seconds);
    return 0;
}

static int radar_parameters_equal(const struct radar_parameters *left,
                                  const struct radar_parameters *right)
{
    if (!left->valid || !right->valid)
        return 0;

    return left->maximum_gate == right->maximum_gate &&
           left->moving_maximum_gate == right->moving_maximum_gate &&
           left->stationary_maximum_gate ==
               right->stationary_maximum_gate &&
           left->nobody_seconds == right->nobody_seconds &&
           memcmp(left->moving_sensitivity,
                  right->moving_sensitivity,
                  sizeof(left->moving_sensitivity)) == 0 &&
           memcmp(left->stationary_sensitivity,
                  right->stationary_sensitivity,
                  sizeof(left->stationary_sensitivity)) == 0;
}

static void write_calibration_record(const struct rear_monitor *monitor)
{
    FILE *file = fopen(CALIBRATION_RECORD_PATH, "w");
    size_t gate;

    if (!file) {
        syslog(LOG_ERR, "open %s failed: %s", CALIBRATION_RECORD_PATH,
               strerror(errno));
        return;
    }

    fprintf(file, "state=%d seconds=%u parameters_changed=%d\n",
            monitor->calibration, monitor->calibration_seconds,
            monitor->parameters_changed);
    if (monitor->parameters_before.valid) {
        fprintf(file, "before moving_max=%u stationary_max=%u nobody=%u\n",
                monitor->parameters_before.moving_maximum_gate,
                monitor->parameters_before.stationary_maximum_gate,
                monitor->parameters_before.nobody_seconds);
        fprintf(file, "before_moving=");
        for (gate = 0; gate <= LD2410B_MAX_GATE; ++gate)
            fprintf(file, "%s%u", gate ? "," : "",
                    monitor->parameters_before.moving_sensitivity[gate]);
        fprintf(file, "\nbefore_stationary=");
        for (gate = 0; gate <= LD2410B_MAX_GATE; ++gate)
            fprintf(file, "%s%u", gate ? "," : "",
                    monitor->parameters_before.stationary_sensitivity[gate]);
        fputc('\n', file);
    }
    if (monitor->parameters_after.valid) {
        fprintf(file, "after moving_max=%u stationary_max=%u nobody=%u\n",
                monitor->parameters_after.moving_maximum_gate,
                monitor->parameters_after.stationary_maximum_gate,
                monitor->parameters_after.nobody_seconds);
        fprintf(file, "after_moving=");
        for (gate = 0; gate <= LD2410B_MAX_GATE; ++gate)
            fprintf(file, "%s%u", gate ? "," : "",
                    monitor->parameters_after.moving_sensitivity[gate]);
        fprintf(file, "\nafter_stationary=");
        for (gate = 0; gate <= LD2410B_MAX_GATE; ++gate)
            fprintf(file, "%s%u", gate ? "," : "",
                    monitor->parameters_after.stationary_sensitivity[gate]);
        fputc('\n', file);
    }
    if (fclose(file) != 0)
        syslog(LOG_ERR, "close %s failed: %s", CALIBRATION_RECORD_PATH,
               strerror(errno));
}

static void load_calibration_record(struct rear_monitor *monitor)
{
    FILE *file = fopen(CALIBRATION_RECORD_PATH, "r");
    int state;

    if (!file)
        return;
    if (fscanf(file, "state=%d seconds=%u parameters_changed=%d",
               &state, &monitor->calibration_seconds,
               &monitor->parameters_changed) == 3 &&
        state >= CALIBRATION_IDLE && state <= CALIBRATION_UNKNOWN)
        monitor->calibration = (enum calibration_state)state;
    fclose(file);
}

static void finish_calibration_if_needed(int uart_fd,
                                         struct rear_monitor *monitor)
{
    if (monitor->snapshot_after_pending) {
        memset(&monitor->parameters_after, 0,
               sizeof(monitor->parameters_after));
        if (read_radar_parameters(uart_fd,
                                  &monitor->parameters_after) == 0) {
            if (monitor->parameters_before.valid) {
                monitor->parameters_changed =
                    !radar_parameters_equal(&monitor->parameters_before,
                                            &monitor->parameters_after);
            } else {
                monitor->calibration = CALIBRATION_UNKNOWN;
                monitor->parameters_changed = -1;
            }
        } else {
            monitor->calibration = CALIBRATION_UNKNOWN;
            monitor->parameters_changed = -1;
        }
        monitor->snapshot_after_pending = 0;
        monitor->record_pending = 1;
    }

    if (monitor->record_pending) {
        write_calibration_record(monitor);
        monitor->record_pending = 0;
    }
}
