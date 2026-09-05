#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <signal.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#define SNAPSHOT_PATH "/run/industrial_safety/system_state.json"
#define DATABASE_PATH "/userdata/industrial_safety/industrial_safety.db"
#define ABNORMAL_EXIT_MARKER "/userdata/industrial_safety/.sqlite_eventd_abnormal"

#define SNAPSHOT_CAPACITY 32768U
#define JSON_OBJECT_CAPACITY 4096U
#define STATE_CAPACITY 32U
#define DETAIL_CAPACITY 256U
#define EVENT_DATETIME_CAPACITY 32U

/* Candidate defaults: light values must be calibrated on the board before acceptance. */
#define LIGHT_HIGH_THRESHOLD_LUX 1000.0
#define LIGHT_CLEAR_THRESHOLD_LUX 800.0
#define TEMPERATURE_HIGH_THRESHOLD_C 40.0
#define TEMPERATURE_CLEAR_THRESHOLD_C 38.0
#define CONDITION_DEBOUNCE_MS 4000LL

struct threshold_config {
    bool temperature_high_enabled;
    double temperature_high;
    bool humidity_high_enabled;
    double humidity_high;
    bool lux_low_enabled;
    double lux_low;
    bool lux_high_enabled;
    double lux_high;
};

struct debounce_tracker {
    bool seen;
    bool active;
    bool pending;
    bool pending_value;
    long long pending_since_ms;
};

struct string_tracker {
    bool seen;
    bool health_known;
    bool healthy;
    char value[STATE_CAPACITY];
};

struct bool_tracker {
    bool seen;
    bool value;
};

struct event_state {
    struct string_tracker door;
    struct string_tracker camera;
    struct string_tracker ai;
    struct string_tracker daemon;
    struct string_tracker radar;
    struct string_tracker sht3x;
    struct string_tracker bh1750;
    struct bool_tracker ai_alarm;
    struct bool_tracker radar_alarm;
    struct bool_tracker intrusion;
    struct bool_tracker temperature_high;
    struct bool_tracker humidity_high;
    struct bool_tracker lux_low;
    struct bool_tracker lux_high;
    struct debounce_tracker temperature;
    struct debounce_tracker light;
    struct debounce_tracker environment;
    struct debounce_tracker suspected_fire;
};

static volatile sig_atomic_t g_stop_requested;
static long long g_event_time_ms;
static long long g_last_snapshot_time_ms;

static void request_stop(int signal_number)
{
    (void)signal_number;
    g_stop_requested = 1;
}

static void log_message(int priority, const char *format, ...)
{
    char message[DETAIL_CAPACITY];
    va_list arguments;

    va_start(arguments, format);
    (void)vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);

    syslog(priority, "%s", message);
    (void)fprintf(stderr, "sqlite_eventd: %s\n", message);
    (void)fflush(stderr);
}

static long long monotonic_ms(void)
{
    struct timespec timestamp;

    if (clock_gettime(CLOCK_MONOTONIC, &timestamp) != 0)
        return 0;

    return (long long)timestamp.tv_sec * 1000LL +
           (long long)timestamp.tv_nsec / 1000000LL;
}

static long long current_event_time_ms(void)
{
    return g_event_time_ms != 0 ? g_event_time_ms : monotonic_ms();
}

static bool format_event_datetime(char *buffer, size_t capacity)
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
    FILE *file;
    size_t length;

    file = fopen(path, "r");
    if (file == NULL)
        return -1;

    length = fread(buffer, 1U, capacity - 1U, file);
    if (ferror(file) != 0 || fclose(file) != 0)
        return -1;

    buffer[length] = '\0';
    return length > 0U ? 0 : -1;
}

static const char *find_json_value(const char *json, const char *key)
{
    char needle[STATE_CAPACITY + 4U];
    const char *position;

    if (snprintf(needle, sizeof(needle), "\"%s\"", key) >=
        (int)sizeof(needle))
        return NULL;

    position = strstr(json, needle);
    if (position == NULL)
        return NULL;

    position = strchr(position + strlen(needle), ':');
    if (position == NULL)
        return NULL;

    position++;
    while (*position == ' ' || *position == '\t' || *position == '\n')
        position++;

    return position;
}

static int json_string_value(const char *json,
                             const char *key,
                             char *value,
                             size_t capacity)
{
    const char *position;
    const char *end;
    size_t length;

    position = find_json_value(json, key);
    if (position == NULL || *position != '\"')
        return -1;

    position++;
    end = strchr(position, '\"');
    if (end == NULL)
        return -1;

    length = (size_t)(end - position);
    if (length + 1U > capacity)
        return -1;

    memcpy(value, position, length);
    value[length] = '\0';
    return 0;
}

static int json_bool_value(const char *json, const char *key, bool *value)
{
    const char *position = find_json_value(json, key);

    if (position == NULL)
        return -1;

    if (strncmp(position, "true", 4U) == 0) {
        *value = true;
        return 0;
    }

    if (strncmp(position, "false", 5U) == 0) {
        *value = false;
        return 0;
    }

    return -1;
}

static int json_number_value(const char *json, const char *key, double *value)
{
    const char *position = find_json_value(json, key);
    char *end;
    double parsed;

    if (position == NULL || strncmp(position, "null", 4U) == 0)
        return -1;

    errno = 0;
    parsed = strtod(position, &end);
    if (errno != 0 || end == position)
        return -1;

    *value = parsed;
    return 0;
}

static int json_object_value(const char *json,
                             const char *key,
                             char *value,
                             size_t capacity)
{
    const char *position = find_json_value(json, key);
    const char *cursor;
    int depth = 0;
    size_t length;

    if (position == NULL || *position != '{')
        return -1;

    for (cursor = position; *cursor != '\0'; cursor++) {
        if (*cursor == '{')
            depth++;
        else if (*cursor == '}')
            depth--;

        if (depth == 0) {
            length = (size_t)(cursor - position + 1);
            if (length + 1U > capacity)
                return -1;
            memcpy(value, position, length);
            value[length] = '\0';
            return 0;
        }
    }

    return -1;
}

static int database_execute(sqlite3 *database, const char *sql)
{
    char *error_message = NULL;
    int result = sqlite3_exec(database, sql, NULL, NULL, &error_message);

    if (result != SQLITE_OK) {
        log_message(LOG_ERR, "SQLite error: %s",
                    error_message == NULL ? sqlite3_errmsg(database) : error_message);
        sqlite3_free(error_message);
        return -1;
    }

    return 0;
}

static int database_has_column(sqlite3 *database,
                               const char *table,
                               const char *column)
{
    char sql[DETAIL_CAPACITY];
    sqlite3_stmt *statement = NULL;
    int result;

    if (snprintf(sql, sizeof(sql), "PRAGMA table_info(%s);", table) >=
        (int)sizeof(sql))
        return -1;

    result = sqlite3_prepare_v2(database, sql, -1, &statement, NULL);
    if (result != SQLITE_OK) {
        log_message(LOG_ERR, "SQLite schema query failed: %s",
                    sqlite3_errmsg(database));
        return -1;
    }

    while ((result = sqlite3_step(statement)) == SQLITE_ROW) {
        const unsigned char *name = sqlite3_column_text(statement, 1);

        if (name != NULL && strcmp((const char *)name, column) == 0) {
            sqlite3_finalize(statement);
            return 1;
        }
    }

    sqlite3_finalize(statement);
    if (result != SQLITE_DONE) {
        log_message(LOG_ERR, "SQLite schema query failed: %s",
                    sqlite3_errmsg(database));
        return -1;
    }

    return 0;
}

static int database_create_schema(sqlite3 *database)
{
    static const char schema[] =
        "CREATE TABLE IF NOT EXISTS events ("
        "event_id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "event_time_ms INTEGER NOT NULL,"
        "event_datetime TEXT,"
        "source TEXT NOT NULL,"
        "event_type TEXT NOT NULL,"
        "level TEXT NOT NULL,"
        "active INTEGER NOT NULL,"
        "value REAL,"
        "threshold REAL,"
        "detail TEXT,"
        "evidence_path TEXT"
        ");"
        "CREATE INDEX IF NOT EXISTS events_time_idx "
        "ON events(event_time_ms DESC);";
    int has_event_datetime;
    int has_evidence_path;

    if (database_execute(database, schema) != 0)
        return -1;

    has_event_datetime = database_has_column(database, "events", "event_datetime");
    if (has_event_datetime < 0)
        return -1;

    if (has_event_datetime == 0 &&
        database_execute(database,
                         "ALTER TABLE events ADD COLUMN event_datetime TEXT;") != 0)
        return -1;

    /* Older deployments predate evidence_path. Keep the daemon compatible
       with those databases so one missing optional column cannot stop all
       event recording. */
    has_evidence_path = database_has_column(database, "events", "evidence_path");
    if (has_evidence_path < 0)
        return -1;
    if (has_evidence_path == 0 &&
        database_execute(database,
                         "ALTER TABLE events ADD COLUMN evidence_path TEXT;") != 0)
        return -1;

    return 0;
}

static int database_insert_event(sqlite3 *database,
                                 const char *source,
                                 const char *event_type,
                                 const char *level,
                                 bool active,
                                 bool has_value,
                                 double value,
                                 bool has_threshold,
                                 double threshold,
                                 const char *detail)
{
    static const char sql[] =
        "INSERT INTO events "
        "(event_time_ms, event_datetime, source, event_type, level, active, value, threshold, detail) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?);";
    sqlite3_stmt *statement = NULL;
    char event_datetime[EVENT_DATETIME_CAPACITY];
    int result;

    if (database_execute(database, "BEGIN IMMEDIATE;") != 0)
        return -1;

    result = sqlite3_prepare_v2(database, sql, -1, &statement, NULL);
    if (result != SQLITE_OK)
        goto rollback;

    (void)sqlite3_bind_int64(statement, 1,
                             (sqlite3_int64)current_event_time_ms());
    if (format_event_datetime(event_datetime, sizeof(event_datetime)))
        (void)sqlite3_bind_text(statement, 2, event_datetime, -1, SQLITE_TRANSIENT);
    else
        (void)sqlite3_bind_null(statement, 2);
    (void)sqlite3_bind_text(statement, 3, source, -1, SQLITE_TRANSIENT);
    (void)sqlite3_bind_text(statement, 4, event_type, -1, SQLITE_TRANSIENT);
    (void)sqlite3_bind_text(statement, 5, level, -1, SQLITE_TRANSIENT);
    (void)sqlite3_bind_int(statement, 6, active ? 1 : 0);
    if (has_value)
        (void)sqlite3_bind_double(statement, 7, value);
    else
        (void)sqlite3_bind_null(statement, 7);
    if (has_threshold)
        (void)sqlite3_bind_double(statement, 8, threshold);
    else
        (void)sqlite3_bind_null(statement, 8);
    (void)sqlite3_bind_text(statement, 9, detail, -1, SQLITE_TRANSIENT);

    result = sqlite3_step(statement);
    sqlite3_finalize(statement);
    statement = NULL;
    if (result != SQLITE_DONE)
        goto rollback;

    if (database_execute(database, "COMMIT;") != 0)
        return -1;

    return 0;

rollback:
    if (statement != NULL)
        sqlite3_finalize(statement);
    log_message(LOG_ERR, "SQLite insert failed: %s", sqlite3_errmsg(database));
    (void)database_execute(database, "ROLLBACK;");
    return -1;
}

static void record_event(sqlite3 *database,
                         const char *source,
                         const char *event_type,
                         const char *level,
                         bool active,
                         bool has_value,
                         double value,
                         bool has_threshold,
                         double threshold,
                         const char *detail)
{
    if (database_insert_event(database, source, event_type, level, active,
                              has_value, value, has_threshold, threshold,
                              detail) != 0) {
        log_message(LOG_ERR, "event lost after SQLite failure: %s/%s",
                    source, event_type);
    }
}

static bool door_sequence_already_recorded(sqlite3 *database,
                                           long long event_sequence)
{
    static const char sql[] =
        "SELECT 1 FROM events "
        "WHERE source = 'door' AND detail LIKE ? LIMIT 1;";
    sqlite3_stmt *statement = NULL;
    char pattern[64];
    int result;

    if (snprintf(pattern, sizeof(pattern), "%%event_sequence=%lld%%",
                 event_sequence) >= (int)sizeof(pattern))
        return false;

    result = sqlite3_prepare_v2(database, sql, -1, &statement, NULL);
    if (result != SQLITE_OK) {
        log_message(LOG_ERR, "SQLite door dedup query failed: %s",
                    sqlite3_errmsg(database));
        return false;
    }

    (void)sqlite3_bind_text(statement, 1, pattern, -1, SQLITE_TRANSIENT);
    result = sqlite3_step(statement);
    sqlite3_finalize(statement);
    return result == SQLITE_ROW;
}

static bool state_is_healthy(const char *source, const char *state)
{
    return strcmp(state, "online") == 0 ||
           (strcmp(source, "ai") == 0 &&
            (strcmp(state, "starting") == 0 ||
             strcmp(state, "running") == 0));
}

static void restore_module_health(sqlite3 *database,
                                  struct string_tracker *tracker,
                                  const char *source)
{
    static const char sql[] =
        "SELECT active FROM events WHERE source = ? "
        "AND event_type IN ('offline', 'recovery') "
        "ORDER BY event_id DESC LIMIT 1;";
    sqlite3_stmt *statement = NULL;
    int result;

    result = sqlite3_prepare_v2(database, sql, -1, &statement, NULL);
    if (result != SQLITE_OK) {
        log_message(LOG_ERR, "SQLite module restore prepare failed: %s",
                    sqlite3_errmsg(database));
        return;
    }
    (void)sqlite3_bind_text(statement, 1, source, -1, SQLITE_TRANSIENT);
    result = sqlite3_step(statement);
    if (result == SQLITE_ROW) {
        tracker->health_known = true;
        tracker->healthy = sqlite3_column_int(statement, 0) == 0;
    } else if (result != SQLITE_DONE) {
        log_message(LOG_ERR, "SQLite module restore query failed: %s",
                    sqlite3_errmsg(database));
    }
    sqlite3_finalize(statement);
}

static void process_module_state(sqlite3 *database,
                                 struct string_tracker *tracker,
                                 const char *source,
                                 const char *current)
{
    char detail[DETAIL_CAPACITY];
    bool was_healthy;
    bool is_healthy;

    is_healthy = state_is_healthy(source, current);
    if (!tracker->seen) {
        tracker->seen = true;
        (void)snprintf(tracker->value, sizeof(tracker->value), "%s", current);
        if (tracker->health_known) {
            if (!tracker->healthy && is_healthy) {
                (void)snprintf(detail, sizeof(detail),
                               "previous=offline current=%s", current);
                record_event(database, source, "recovery", "info", false,
                             false, 0.0, false, 0.0, detail);
            } else if (tracker->healthy && !is_healthy) {
                (void)snprintf(detail, sizeof(detail),
                               "previous=online current=%s", current);
                record_event(database, source, "offline", "warning", true,
                             false, 0.0, false, 0.0, detail);
            }
        } else if (!is_healthy) {
            (void)snprintf(detail, sizeof(detail), "state=%s", current);
            record_event(database, source, "offline", "warning", true,
                         false, 0.0, false, 0.0, detail);
        }
        tracker->health_known = true;
        tracker->healthy = is_healthy;
        return;
    }

    if (strcmp(tracker->value, current) == 0)
        return;

    was_healthy = state_is_healthy(source, tracker->value);
    is_healthy = state_is_healthy(source, current);
    (void)snprintf(detail, sizeof(detail), "previous=%s current=%s",
                   tracker->value, current);
    if (!was_healthy && is_healthy) {
        record_event(database, source, "recovery", "info", false,
                     false, 0.0, false, 0.0, detail);
    } else if (was_healthy && !is_healthy) {
        record_event(database, source, "offline", "warning", true,
                     false, 0.0, false, 0.0, detail);
    }

    (void)snprintf(tracker->value, sizeof(tracker->value), "%s", current);
    tracker->health_known = true;
    tracker->healthy = is_healthy;
}

static bool process_object_state(sqlite3 *database,
                                 struct string_tracker *tracker,
                                 const char *object,
                                 const char *key,
                                 const char *source)
{
    char current[STATE_CAPACITY];

    if (json_string_value(object, key, current, sizeof(current)) != 0)
        return false;

    process_module_state(database, tracker, source, current);
    return true;
}

static void process_door(sqlite3 *database,
                         struct event_state *state,
                         const char *door)
{
    char current[STATE_CAPACITY];
    char detail[DETAIL_CAPACITY];
    bool active;
    double sequence_value;
    bool has_sequence;
    long long event_sequence = 0;

    if (json_string_value(door, "logical_state", current, sizeof(current)) != 0)
        return;
    if (strcmp(current, "active") != 0 && strcmp(current, "inactive") != 0)
        return;

    active = strcmp(current, "active") == 0;
    has_sequence = json_number_value(door, "event_sequence", &sequence_value) == 0;
    if (has_sequence)
        event_sequence = (long long)sequence_value;

    if (!state->door.seen) {
        state->door.seen = true;
        (void)snprintf(state->door.value, sizeof(state->door.value), "%s", current);
        if (!active)
            return;
        if (has_sequence && door_sequence_already_recorded(database, event_sequence))
            return;
        (void)snprintf(detail, sizeof(detail),
                       "logical_state=%s event_sequence=%lld",
                       current, event_sequence);
        record_event(database, "door", "open", "alarm", true,
                     false, 0.0, false, 0.0, detail);
        return;
    }
    if (strcmp(state->door.value, current) == 0)
        return;

    if (has_sequence) {
        (void)snprintf(detail, sizeof(detail),
                       "logical_state=%s event_sequence=%lld",
                       current, event_sequence);
    } else {
        (void)snprintf(detail, sizeof(detail), "logical_state=%s", current);
    }
    record_event(database, "door", active ? "open" : "close",
                 active ? "alarm" : "info", active,
                 false, 0.0, false, 0.0, detail);
    (void)snprintf(state->door.value, sizeof(state->door.value), "%s", current);
}

static bool process_alarm(sqlite3 *database,
                          struct bool_tracker *tracker,
                          const char *alarm,
                          const char *key,
                          const char *source)
{
    bool current;
    char detail[DETAIL_CAPACITY];

    if (json_bool_value(alarm, key, &current) != 0)
        return false;

    if (!tracker->seen) {
        tracker->seen = true;
        tracker->value = current;
        return true;
    }
    if (tracker->value == current)
        return true;

    (void)snprintf(detail, sizeof(detail), "%s=%s", key,
                   current ? "true" : "false");
    record_event(database, source, current ? "event" : "clear",
                 current ? "alarm" : "info", current,
                 false, 0.0, false, 0.0, detail);
    tracker->value = current;
    return true;
}

static void process_intrusion(sqlite3 *database,
                              struct bool_tracker *tracker,
                              bool ai_online,
                              bool radar_online,
                              bool ai_alarm,
                              bool radar_alarm)
{
    bool active;
    static const char sql[] =
        "SELECT active FROM events "
        "WHERE source = 'intrusion' "
        "ORDER BY event_id DESC LIMIT 1;";
    sqlite3_stmt *statement = NULL;
    char detail[DETAIL_CAPACITY];
    int result;

    if (!ai_online || !radar_online)
        return;

    active = ai_alarm || radar_alarm;
    if (!tracker->seen) {
        tracker->seen = true;
        tracker->value = false;
        result = sqlite3_prepare_v2(database, sql, -1, &statement, NULL);
        if (result == SQLITE_OK) {
            result = sqlite3_step(statement);
            if (result == SQLITE_ROW) {
                tracker->value = sqlite3_column_int(statement, 0) != 0;
            } else if (result != SQLITE_DONE) {
                log_message(LOG_ERR, "SQLite intrusion state query failed: %s",
                            sqlite3_errmsg(database));
            }
            sqlite3_finalize(statement);
        } else {
            log_message(LOG_ERR, "SQLite intrusion state prepare failed: %s",
                        sqlite3_errmsg(database));
        }
    } else if (tracker->value == active) {
        return;
    }

    if (tracker->value == active)
        return;

    (void)snprintf(detail, sizeof(detail),
                   "ai_online=%s radar_online=%s ai_alarm=%s radar_alarm=%s",
                   ai_online ? "true" : "false",
                   radar_online ? "true" : "false",
                   ai_alarm ? "true" : "false",
                   radar_alarm ? "true" : "false");
    record_event(database, "intrusion", active ? "enter" : "clear",
                 active ? "alarm" : "info", active,
                 false, 0.0, false, 0.0, detail);
    tracker->value = active;
}

static void process_high_threshold(sqlite3 *database,
                                   struct bool_tracker *tracker,
                                   bool enabled,
                                   double threshold,
                                   const char *source,
                                   const char *event_name,
                                   const char *object,
                                   const char *key)
{
    double value;
    bool active;
    char detail[DETAIL_CAPACITY];

    if (!enabled || json_number_value(object, key, &value) != 0)
        return;

    active = value > threshold;
    if (!tracker->seen) {
        tracker->seen = true;
        tracker->value = active;
        return;
    }
    if (tracker->value == active)
        return;

    (void)snprintf(detail, sizeof(detail), "%s=%.3f threshold=%.3f",
                   key, value, threshold);
    record_event(database, source,
                 active ? event_name : "recovery",
                 active ? "warning" : "info", active,
                 true, value, true, threshold, detail);
    tracker->value = active;
}

static void process_low_threshold(sqlite3 *database,
                                  struct bool_tracker *tracker,
                                  bool enabled,
                                  double threshold,
                                  const char *source,
                                  const char *event_name,
                                  const char *object,
                                  const char *key)
{
    double value;
    bool active;
    char detail[DETAIL_CAPACITY];

    if (!enabled || json_number_value(object, key, &value) != 0)
        return;

    active = value < threshold;
    if (!tracker->seen) {
        tracker->seen = true;
        tracker->value = active;
        return;
    }
    if (tracker->value == active)
        return;

    (void)snprintf(detail, sizeof(detail), "%s=%.3f threshold=%.3f",
                   key, value, threshold);
    record_event(database, source,
                 active ? event_name : "recovery",
                 active ? "warning" : "info", active,
                 true, value, true, threshold, detail);
    tracker->value = active;
}

static bool load_threshold(const char *name, bool *enabled, double *value)
{
    const char *text = getenv(name);
    char *end;
    double parsed;

    if (text == NULL || *text == '\0')
        return true;

    errno = 0;
    parsed = strtod(text, &end);
    if (errno != 0 || end == text || *end != '\0') {
        log_message(LOG_ERR, "invalid %s=%s", name, text);
        return false;
    }

    *enabled = true;
    *value = parsed;
    return true;
}

static bool load_thresholds(struct threshold_config *config)
{
    config->temperature_high_enabled = true;
    config->temperature_high = TEMPERATURE_HIGH_THRESHOLD_C;
    config->lux_high_enabled = true;
    config->lux_high = LIGHT_HIGH_THRESHOLD_LUX;
    return load_threshold("SQLITE_EVENTD_HUMIDITY_HIGH",
                          &config->humidity_high_enabled,
                          &config->humidity_high) &&
           load_threshold("SQLITE_EVENTD_LUX_LOW",
                          &config->lux_low_enabled,
                          &config->lux_low);
}

static bool sensor_state_online(const struct string_tracker *tracker,
                                const char *source)
{
    return tracker->seen && state_is_healthy(source, tracker->value);
}

static void process_debounced(sqlite3 *database,
                              struct debounce_tracker *tracker,
                              bool valid, bool candidate,
                              const char *source, const char *event_name,
                              const char *recovery_name,
                              bool has_value, double value,
                              double active_threshold,
                              double recovery_threshold,
                              const char *detail_key)
{
    long long now = current_event_time_ms();
    char detail[DETAIL_CAPACITY];

    if (!valid) {
        tracker->pending = false;
        return;
    }
    if (!tracker->seen) {
        tracker->seen = true;
        tracker->active = false;
    }
    if (candidate == tracker->active) {
        tracker->pending = false;
        return;
    }
    if (!tracker->pending || tracker->pending_value != candidate) {
        tracker->pending = true;
        tracker->pending_value = candidate;
        tracker->pending_since_ms = now;
        return;
    }
    if (now - tracker->pending_since_ms < CONDITION_DEBOUNCE_MS)
        return;
    (void)snprintf(detail, sizeof(detail), "%s=%.3f threshold=%.3f duration_ms=%lld",
                   detail_key, value,
                   candidate ? active_threshold : recovery_threshold,
                   now - tracker->pending_since_ms);
    record_event(database, source, candidate ? event_name : recovery_name,
                 candidate ? "warning" : "info", candidate,
                 has_value, value, has_value,
                 candidate ? active_threshold : recovery_threshold, detail);
    tracker->active = candidate;
    tracker->pending = false;
}

static void restore_debounce_state(sqlite3 *database,
                                   struct debounce_tracker *tracker,
                                   const char *source,
                                   const char *active_event,
                                   const char *clear_event)
{
    static const char sql[] =
        "SELECT active FROM events WHERE source = ? "
        "AND event_type IN (?, ?) ORDER BY event_id DESC LIMIT 1;";
    sqlite3_stmt *statement = NULL;
    int result;

    result = sqlite3_prepare_v2(database, sql, -1, &statement, NULL);
    if (result != SQLITE_OK) {
        log_message(LOG_ERR, "SQLite state restore prepare failed: %s",
                    sqlite3_errmsg(database));
        return;
    }
    (void)sqlite3_bind_text(statement, 1, source, -1, SQLITE_TRANSIENT);
    (void)sqlite3_bind_text(statement, 2, active_event, -1, SQLITE_TRANSIENT);
    (void)sqlite3_bind_text(statement, 3, clear_event, -1, SQLITE_TRANSIENT);
    result = sqlite3_step(statement);
    if (result == SQLITE_ROW) {
        tracker->seen = true;
        tracker->active = sqlite3_column_int(statement, 0) != 0;
    } else if (result != SQLITE_DONE) {
        log_message(LOG_ERR, "SQLite state restore query failed: %s",
                    sqlite3_errmsg(database));
    }
    sqlite3_finalize(statement);
}

static void process_snapshot(sqlite3 *database,
                             struct event_state *state,
                             const struct threshold_config *config,
                             const char *snapshot)
{
    char alarm[JSON_OBJECT_CAPACITY];
    char ai[JSON_OBJECT_CAPACITY];
    char ld2410[JSON_OBJECT_CAPACITY];
    char sht3x[JSON_OBJECT_CAPACITY];
    char bh1750[JSON_OBJECT_CAPACITY];
    char door[JSON_OBJECT_CAPACITY];
    double snapshot_monotonic;
    double temperature = 0.0;
    double lux = 0.0;
    bool temperature_valid = false;
    bool lux_valid = false;
    bool temperature_high = false;
    bool light_high = false;
    bool have_ai_alarm = false;
    bool have_radar_alarm = false;
    bool have_ai_state = false;
    bool have_radar_state = false;

    if (json_number_value(snapshot, "monotonic_ms", &snapshot_monotonic) == 0) {
        long long candidate_time = (long long)snapshot_monotonic;
        if (candidate_time >= g_last_snapshot_time_ms) {
            g_last_snapshot_time_ms = candidate_time;
            g_event_time_ms = candidate_time;
        }
    }

    if (json_object_value(snapshot, "ai", ai, sizeof(ai)) == 0) {
        (void)process_object_state(database, &state->camera, ai,
                                   "camera_state", "camera");
        have_ai_state = process_object_state(database, &state->ai, ai,
                                             "state", "ai");
    }
    if (!have_ai_state)
        have_ai_state = process_object_state(database, &state->ai, snapshot,
                                             "ai_state", "ai");
    if (json_object_value(snapshot, "ld2410", ld2410, sizeof(ld2410)) == 0) {
        have_radar_state = process_object_state(database, &state->radar,
                                                ld2410, "state", "radar");
    }
    if (json_object_value(snapshot, "alarm", alarm, sizeof(alarm)) == 0) {
        (void)process_object_state(database, &state->daemon, alarm, "state", "daemon");
        /* Alarm fields are untrusted while their source health is offline. */
        if (have_ai_state && state_is_healthy("ai", state->ai.value)) {
            have_ai_alarm = process_alarm(database, &state->ai_alarm, alarm,
                                          "ai_alarm", "ai");
        }
        if (have_radar_state && state_is_healthy("radar", state->radar.value)) {
            have_radar_alarm = process_alarm(database, &state->radar_alarm, alarm,
                                             "radar_alarm", "radar");
        }
    }
    if (have_ai_state && have_radar_state &&
        have_ai_alarm && have_radar_alarm) {
        process_intrusion(database, &state->intrusion,
                          state_is_healthy("ai", state->ai.value),
                          state_is_healthy("radar", state->radar.value),
                          state->ai_alarm.value, state->radar_alarm.value);
    }
    if (json_object_value(snapshot, "sht3x", sht3x, sizeof(sht3x)) == 0) {
        process_object_state(database, &state->sht3x, sht3x, "state", "sht3x");
        temperature_valid = sensor_state_online(&state->sht3x, "sht3x") &&
                            json_number_value(sht3x, "temperature_c", &temperature) == 0;
        temperature_high = temperature_valid &&
                           (state->temperature.active ?
                            temperature > TEMPERATURE_CLEAR_THRESHOLD_C :
                            temperature >= config->temperature_high);
        process_debounced(database, &state->temperature,
                          temperature_valid, temperature_high, "temperature",
                          "overheat", "recovery", temperature_valid, temperature,
                          config->temperature_high,
                          TEMPERATURE_CLEAR_THRESHOLD_C, "temperature_c");
        process_high_threshold(database, &state->humidity_high,
                               config->humidity_high_enabled,
                               config->humidity_high, "sht3x",
                               "humidity_high", sht3x, "humidity_rh");
    }
    if (json_object_value(snapshot, "bh1750", bh1750, sizeof(bh1750)) == 0) {
        process_object_state(database, &state->bh1750, bh1750, "state", "bh1750");
        lux_valid = sensor_state_online(&state->bh1750, "bh1750") &&
                    json_number_value(bh1750, "value_lux", &lux) == 0;
        light_high = lux_valid &&
                     (state->light.active ? lux > LIGHT_CLEAR_THRESHOLD_LUX :
                                            lux >= config->lux_high);
        process_debounced(database, &state->light, lux_valid, light_high,
                          "light", "overexposure", "recovery", lux_valid, lux,
                          config->lux_high, LIGHT_CLEAR_THRESHOLD_LUX,
                          "value_lux");
        process_low_threshold(database, &state->lux_low,
                              config->lux_low_enabled, config->lux_low,
                              "bh1750", "lux_low", bh1750, "value_lux");
    }
    process_debounced(database, &state->environment,
                      temperature_valid && lux_valid,
                      temperature_high || light_high,
                      "environment", "event", "clear",
                      false, 0.0, 0.0, 0.0, "environment_alarm");
    process_debounced(database, &state->suspected_fire,
                      temperature_valid && lux_valid,
                      temperature_high && light_high,
                      "environment",
                      "suspected_fire", "suspected_fire_clear",
                      false, 0.0, 0.0, 0.0,
                      "suspected_fire");
    if (json_object_value(snapshot, "door", door, sizeof(door)) == 0)
        process_door(database, state, door);
}

int main(void)
{
    sqlite3 *database = NULL;
    struct event_state state;
    struct threshold_config config;
    char snapshot[SNAPSHOT_CAPACITY];
    bool snapshot_failed = false;
    bool startup_recorded = false;
    bool abnormal_recovery_pending;
    int result;

    memset(&state, 0, sizeof(state));
    memset(&config, 0, sizeof(config));
    openlog("sqlite_eventd", LOG_PID, LOG_DAEMON);

    if (!load_thresholds(&config))
        return EXIT_FAILURE;

    (void)signal(SIGTERM, request_stop);
    (void)signal(SIGINT, request_stop);

    result = sqlite3_open_v2(DATABASE_PATH, &database,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                             NULL);
    if (result != SQLITE_OK) {
        log_message(LOG_ERR, "cannot open %s: %s", DATABASE_PATH,
                    database == NULL ? "unknown SQLite error" : sqlite3_errmsg(database));
        if (database != NULL)
            sqlite3_close(database);
        return EXIT_FAILURE;
    }

    sqlite3_busy_timeout(database, 100);
    if (database_create_schema(database) != 0) {
        sqlite3_close(database);
        return EXIT_FAILURE;
    }

    restore_module_health(database, &state.camera, "camera");
    restore_module_health(database, &state.ai, "ai");
    restore_module_health(database, &state.daemon, "daemon");
    restore_module_health(database, &state.radar, "radar");
    restore_module_health(database, &state.sht3x, "sht3x");
    restore_module_health(database, &state.bh1750, "bh1750");

    restore_debounce_state(database, &state.temperature, "temperature",
                           "overheat", "recovery");
    restore_debounce_state(database, &state.light, "light",
                           "overexposure", "recovery");
    restore_debounce_state(database, &state.environment, "environment",
                           "event", "clear");
    restore_debounce_state(database, &state.suspected_fire, "environment",
                           "suspected_fire", "suspected_fire_clear");

    abnormal_recovery_pending = access(ABNORMAL_EXIT_MARKER, F_OK) == 0;

    while (!g_stop_requested) {
        if (read_text_file(SNAPSHOT_PATH, snapshot, sizeof(snapshot)) != 0) {
            if (!snapshot_failed) {
                record_event(database, "system", "snapshot_invalid", "warning", true,
                             false, 0.0, false, 0.0, "snapshot unavailable or empty");
                log_message(LOG_WARNING, "snapshot unavailable: %s", SNAPSHOT_PATH);
                snapshot_failed = true;
            }
        } else {
            if (!startup_recorded) {
                if (abnormal_recovery_pending) {
                    record_event(database, "system", "abnormal_exit", "warning", true,
                                 false, 0.0, false, 0.0,
                                 "sqlite_eventd supervisor observed non-zero exit");
                    record_event(database, "system", "recovery", "info", false,
                                 false, 0.0, false, 0.0,
                                 "sqlite_eventd restarted after abnormal exit");
                    if (unlink(ABNORMAL_EXIT_MARKER) != 0)
                        log_message(LOG_WARNING, "cannot remove recovery marker: %s",
                                    strerror(errno));
                } else {
                    record_event(database, "system", "startup", "info", true,
                                 false, 0.0, false, 0.0, "sqlite_eventd started");
                }
                startup_recorded = true;
            }
            if (snapshot_failed) {
                record_event(database, "system", "snapshot_recovery", "info", false,
                             false, 0.0, false, 0.0, "snapshot readable again");
                snapshot_failed = false;
            }
            process_snapshot(database, &state, &config, snapshot);
        }
        sleep(1);
    }

    g_event_time_ms = monotonic_ms();
    record_event(database, "system", "shutdown", "info", false,
                 false, 0.0, false, 0.0, "sqlite_eventd stopped");
    sqlite3_close(database);
    closelog();
    return EXIT_SUCCESS;
}
