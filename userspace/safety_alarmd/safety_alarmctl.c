#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define CONTROL_SOCKET_PATH "/run/safety_alarmd.sock"
#define RESPONSE_BUFFER_SIZE 2048U

static void print_usage(const char *program)
{
    fprintf(stderr,
            "usage: %s rear "
            "{enable|disable|status|raw|energy|gates|resolution|config}\n"
            "       %s snapshot\n"
            "       %s rear calibrate <seconds>\n"
            "       %s rear set-nobody <1-60>\n"
            "       %s rear set-resolution <750|200>\n"
            "       %s rear set-range <2-8>\n"
            "       %s ai {on|off}\n",
            program, program, program, program, program, program, program);
}

static int parse_seconds(const char *text, unsigned long minimum,
                         unsigned long maximum, unsigned long *seconds)
{
    char *end;
    unsigned long value;

    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || text == end || *end != '\0' ||
        value < minimum || value > maximum)
        return -1;

    *seconds = value;
    return 0;
}

static int command_from_args(int argc, char *argv[],
                             char *command, size_t command_size)
{
    unsigned long seconds;
    int length;

    if (argc == 2 && strcmp(argv[1], "snapshot") == 0) {
        length = snprintf(command, command_size, "snapshot\n");
    } else if (argc == 3 && strcmp(argv[1], "rear") == 0 &&
        (strcmp(argv[2], "enable") == 0 ||
         strcmp(argv[2], "disable") == 0 ||
         strcmp(argv[2], "status") == 0 ||
         strcmp(argv[2], "raw") == 0 ||
         strcmp(argv[2], "energy") == 0 ||
         strcmp(argv[2], "gates") == 0 ||
         strcmp(argv[2], "resolution") == 0 ||
         strcmp(argv[2], "config") == 0)) {
        length = snprintf(command, command_size, "rear %s\n", argv[2]);
    } else if (argc == 4 && strcmp(argv[1], "rear") == 0 &&
               strcmp(argv[2], "calibrate") == 0 &&
               parse_seconds(argv[3], 1U, 65535U, &seconds) == 0) {
        length = snprintf(command, command_size,
                          "rear calibrate %lu\n", seconds);
    } else if (argc == 4 && strcmp(argv[1], "rear") == 0 &&
               strcmp(argv[2], "set-nobody") == 0 &&
               parse_seconds(argv[3], 1U, 60U, &seconds) == 0) {
        length = snprintf(command, command_size,
                          "rear set-nobody %lu\n", seconds);
    } else if (argc == 4 && strcmp(argv[1], "rear") == 0 &&
               strcmp(argv[2], "set-resolution") == 0 &&
               parse_seconds(argv[3], 200U, 750U, &seconds) == 0 &&
               (seconds == 200U || seconds == 750U)) {
        length = snprintf(command, command_size,
                          "rear set-resolution %lu\n", seconds);
    } else if (argc == 4 && strcmp(argv[1], "rear") == 0 &&
               strcmp(argv[2], "set-range") == 0 &&
               parse_seconds(argv[3], 2U, 8U, &seconds) == 0) {
        length = snprintf(command, command_size,
                          "rear set-range %lu\n", seconds);
    } else if (argc == 3 && strcmp(argv[1], "ai") == 0 &&
               (strcmp(argv[2], "on") == 0 ||
                strcmp(argv[2], "off") == 0)) {
        length = snprintf(command, command_size, "ai %s\n", argv[2]);
    } else {
        print_usage(argv[0]);
        return -1;
    }

    if (length < 0 || (size_t)length >= command_size) {
        fprintf(stderr, "command is too long\n");
        return -1;
    }

    return 0;
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

int main(int argc, char *argv[])
{
    struct sockaddr_un address;
    char command[64];
    char response[RESPONSE_BUFFER_SIZE];
    ssize_t bytes;
    int fd = -1;
    int rc = EXIT_FAILURE;

    if (command_from_args(argc, argv, command, sizeof(command)) < 0)
        return EXIT_FAILURE;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        goto out;
    }

    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s",
             CONTROL_SOCKET_PATH);

    if (connect(fd, (const struct sockaddr *)&address, sizeof(address)) < 0) {
        fprintf(stderr, "connect %s failed: %s\n",
                CONTROL_SOCKET_PATH, strerror(errno));
        goto out;
    }

    if (write_all(fd, command, strlen(command)) < 0) {
        fprintf(stderr, "write command failed: %s\n", strerror(errno));
        goto out;
    }

    do {
        bytes = read(fd, response, sizeof(response) - 1U);
    } while (bytes < 0 && errno == EINTR);

    if (bytes < 0) {
        fprintf(stderr, "read response failed: %s\n", strerror(errno));
        goto out;
    }
    if (bytes == 0) {
        fprintf(stderr, "control daemon closed without a response\n");
        goto out;
    }

    response[bytes] = '\0';
    fputs(response, stdout);
    rc = strncmp(response, "error ", 6U) == 0 ?
         EXIT_FAILURE : EXIT_SUCCESS;
out:
    if (fd >= 0)
        close(fd);
    return rc;
}
