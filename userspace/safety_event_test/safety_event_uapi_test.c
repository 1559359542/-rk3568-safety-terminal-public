#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "../../drivers/safety_event/safety_event_uapi.h"

#define SAFETY_EVENT_DEVICE "/dev/safety_event"
#define POLL_TIMEOUT_MS 5000

static int check_api_version(int fd)
{
    __u32 version = 0;

    if (ioctl(fd, SAFETY_EVENT_IOC_GET_API_VERSION, &version) < 0) {
        fprintf(stderr, "GET_API_VERSION 失败: %s\n", strerror(errno));
        return -1;
    }

    printf("api_version=%u\n", version);
    if (version != SAFETY_EVENT_API_VERSION) {
        fprintf(stderr, "API 版本不匹配，期望 %u\n",
                SAFETY_EVENT_API_VERSION);
        return -1;
    }

    return 0;
}

static int wait_and_read_event(int fd)
{
    struct safety_event_record event;
    struct pollfd pfd = {
        .fd = fd,
        .events = POLLIN,
    };
    ssize_t bytes;
    int poll_rc;

    printf("waiting for an event...\n");
    fflush(stdout);

    poll_rc = poll(&pfd, 1, POLL_TIMEOUT_MS);
    if (poll_rc < 0) {
        fprintf(stderr, "poll failed: %s\n", strerror(errno));
        return -1;
    }
    if (poll_rc == 0) {
        fprintf(stderr, "poll timed out without an event\n");
        return -1;
    }
    if ((pfd.revents & POLLIN) == 0) {
        fprintf(stderr, "poll returned no POLLIN, revents=0x%x\n",
                pfd.revents);
        return -1;
    }

    bytes = read(fd, &event, sizeof(event));
    if (bytes < 0) {
        fprintf(stderr, "read failed: %s\n", strerror(errno));
        return -1;
    }
    if (bytes != (ssize_t)sizeof(event)) {
        fprintf(stderr, "read length %zd, expected %zu\n",
                bytes, sizeof(event));
        return -1;
    }

    printf("sequence=%u type=%u source=%u state=%u timestamp_ns=%" PRIu64 "\n",
           event.sequence, event.type, event.source, event.state,
           (uint64_t)event.timestamp_ns);
    return 0;
}

static int inject_only_event(int fd)
{
    struct safety_event_record inject = {
        .type = SAFETY_EVENT_TYPE_TEST,
        .source = SAFETY_EVENT_SOURCE_TEST,
        .state = SAFETY_EVENT_STATE_UNKNOWN,
        .timestamp_ns = 0,
    };

    if (ioctl(fd, SAFETY_EVENT_IOC_INJECT, &inject) < 0) {
        fprintf(stderr, "INJECT failed: %s\n", strerror(errno));
        return -1;
    }

    printf("test event injected\n");
    return 0;
}

static int inject_and_read_event(int fd)
{
    struct safety_event_record inject = {
        .type = SAFETY_EVENT_TYPE_TEST,
        .source = SAFETY_EVENT_SOURCE_TEST,
        .state = SAFETY_EVENT_STATE_UNKNOWN,
        .timestamp_ns = 0, /* 时间戳由驱动发布事件时填写 */
    };
    struct safety_event_record event;
    struct pollfd pfd = {
        .fd = fd,
        .events = POLLIN,
    };
    ssize_t bytes;
    int poll_rc;

    if (ioctl(fd, SAFETY_EVENT_IOC_INJECT, &inject) < 0) {
        fprintf(stderr, "INJECT 失败: %s\n", strerror(errno));
        return -1;
    }

    poll_rc = poll(&pfd, 1, POLL_TIMEOUT_MS);
    if (poll_rc < 0) {
        fprintf(stderr, "poll 失败: %s\n", strerror(errno));
        return -1;
    }
    if (poll_rc == 0) {
        fprintf(stderr, "poll 超时，设备没有变为可读\n");
        return -1;
    }
    if ((pfd.revents & POLLIN) == 0) {
        fprintf(stderr, "poll 未返回 POLLIN，revents=0x%x\n", pfd.revents);
        return -1;
    }

    bytes = read(fd, &event, sizeof(event));
    if (bytes < 0) {
        fprintf(stderr, "read 失败: %s\n", strerror(errno));
        return -1;
    }
    if (bytes != (ssize_t)sizeof(event)) {
        fprintf(stderr, "read 长度异常: %zd，期望 %zu\n",
                bytes, sizeof(event));
        return -1;
    }

    printf("sequence=%u type=%u source=%u state=%u timestamp_ns=%" PRIu64 "\n",
           event.sequence, event.type, event.source, event.state,
           (uint64_t)event.timestamp_ns);
    return 0;
}
static int set_user_alarm(int fd, bool active)
{
    struct safety_event_user_alarm_request request = {
        .active = active ? 1U : 0U,
        .reserved = 0,
    };

    if (ioctl(fd, SAFETY_EVENT_IOC_SET_USER_ALARM, &request) < 0) {
        fprintf(stderr, "SET_USER_ALARM failed: %s\n", strerror(errno));
        return -1;
    }

    printf("user_alarm=%s\n", active ? "on" : "off");
    return 0;
}

int main(int argc, char *argv[])
{
    int fd;
    int rc = EXIT_FAILURE;

    if (argc > 2 ||
        (argc == 2 &&
        strcmp(argv[1], "--inject") != 0 &&
        strcmp(argv[1], "--wait") != 0 &&
        strcmp(argv[1], "--inject-only") != 0 &&
        strcmp(argv[1], "--alarm-on") != 0 &&
        strcmp(argv[1], "--alarm-off") != 0)) {
        fprintf(stderr,"usage: %s [--inject|--wait|--inject-only|--alarm-on|--alarm-off]\n",argv[0]);
        return EXIT_FAILURE;
    }
    fd = open(SAFETY_EVENT_DEVICE, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "打开 %s 失败: %s\n",
                SAFETY_EVENT_DEVICE, strerror(errno));
        return EXIT_FAILURE;
    }

    if (check_api_version(fd) < 0)
        goto out;

    if (argc == 2 && strcmp(argv[1], "--alarm-on") == 0) {
        if (set_user_alarm(fd, true) < 0)
            goto out;
    } else if (argc == 2 && strcmp(argv[1], "--alarm-off") == 0) {
        if (set_user_alarm(fd, false) < 0)
            goto out;
    } else if (argc == 2 && strcmp(argv[1], "--inject") == 0) {
        if (inject_and_read_event(fd) < 0)
            goto out;
    } else if (argc == 2 && strcmp(argv[1], "--wait") == 0) {
        if (wait_and_read_event(fd) < 0)
            goto out;
    } else if (argc == 2 &&
               strcmp(argv[1], "--inject-only") == 0) {
        if (inject_only_event(fd) < 0)
            goto out;
    }

    rc = EXIT_SUCCESS;
out:
    close(fd);
    return rc;
}