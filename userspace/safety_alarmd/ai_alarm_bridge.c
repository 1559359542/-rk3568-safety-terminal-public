#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define CONTROL_SOCKET_PATH "/run/safety_alarmd.sock"
#define LINE_BUFFER_SIZE 512U
#define RESPONSE_BUFFER_SIZE 512U

static volatile sig_atomic_t stop_requested;

static void request_stop(int signum)
{
    (void)signum;
    stop_requested = 1;
}

static int write_all(int fd, const char *buffer, size_t length)
{
    size_t written = 0;

    while (written < length) {
        ssize_t bytes = write(fd, buffer + written, length - written);

        if (bytes > 0) {
            written += (size_t)bytes;
            continue;
        }
        if (bytes < 0 && errno == EINTR)
            continue;

        return -1;
    }

    return 0;
}

static int request_ai_alarm(int active)
{
    struct sockaddr_un address;
    char response[RESPONSE_BUFFER_SIZE];
    const char *command = active ? "ai on\n" : "ai off\n";
    ssize_t bytes;
    int fd = -1;
    int rc = -1;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "ai bridge: socket failed: %s\n", strerror(errno));
        goto out;
    }

    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s",
             CONTROL_SOCKET_PATH);

    if (connect(fd, (const struct sockaddr *)&address, sizeof(address)) < 0) {
        fprintf(stderr, "ai bridge: connect %s failed: %s\n",
                CONTROL_SOCKET_PATH, strerror(errno));
        goto out;
    }

    if (write_all(fd, command, strlen(command)) < 0) {
        fprintf(stderr, "ai bridge: write %s failed: %s",
                active ? "ai on\n" : "ai off\n", strerror(errno));
        goto out;
    }

    do {
        bytes = read(fd, response, sizeof(response) - 1U);
    } while (bytes < 0 && errno == EINTR);

    if (bytes <= 0) {
        fprintf(stderr, "ai bridge: read response failed: %s\n",
                bytes == 0 ? "daemon closed socket" : strerror(errno));
        goto out;
    }

    response[bytes] = '\0';
    if (strncmp(response, "error ", 6U) == 0) {
        fprintf(stderr, "ai bridge: daemon rejected %s: %s",
                active ? "ai on" : "ai off", response);
        goto out;
    }

    fprintf(stderr, "ai bridge: %s", response);
    rc = 0;
out:
    if (fd >= 0)
        close(fd);
    return rc;
}

static int install_signal_handlers(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = request_stop;
    sigemptyset(&action.sa_mask);

    return sigaction(SIGINT, &action, NULL) == 0 &&
           sigaction(SIGTERM, &action, NULL) == 0 ? 0 : -1;
}

int main(void)
{
    char line[LINE_BUFFER_SIZE];
    int ai_alarm_on = 0;
    int rc = EXIT_SUCCESS;

    if (install_signal_handlers() < 0) {
        fprintf(stderr, "ai bridge: sigaction failed: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }

    while (!stop_requested && fgets(line, sizeof(line), stdin) != NULL) {
        int requested_state = -1;

        if (strncmp(line, "INTRUSION_EVENT ", 16U) == 0)
            requested_state = 1;
        else if (strncmp(line, "INTRUSION_CLEAR ", 16U) == 0)
            requested_state = 0;

        if (requested_state >= 0 && requested_state != ai_alarm_on) {
            if (request_ai_alarm(requested_state) < 0) {
                rc = EXIT_FAILURE;
                break;
            }
            ai_alarm_on = requested_state;
        }
    }

    if (ferror(stdin)) {
        fprintf(stderr, "ai bridge: stdin read failed: %s\n", strerror(errno));
        rc = EXIT_FAILURE;
    }

    if (request_ai_alarm(0) < 0)
        rc = EXIT_FAILURE;

    return rc;
}
