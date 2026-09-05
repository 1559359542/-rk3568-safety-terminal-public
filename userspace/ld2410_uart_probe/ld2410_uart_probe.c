#include <asm/ioctls.h>
#include <asm/termbits.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_DEVICE "/dev/ttyS9"
#define LD2410_BAUD 256000U
#define READ_BUFFER_SIZE 256U

static int64_t monotonic_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return -1;
    }

    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int configure_uart_256000(int fd)
{
    struct termios2 config;

    if (ioctl(fd, TCGETS2, &config) != 0) {
        perror("TCGETS2");
        return -1;
    }

    config.c_cflag &= ~(CBAUD | CSIZE | PARENB | CSTOPB);
#ifdef CRTSCTS
    config.c_cflag &= ~CRTSCTS;
#endif
    config.c_cflag |= BOTHER | CS8 | CLOCAL | CREAD;

    config.c_iflag &= ~(IGNBRK | BRKINT | ICRNL | INLCR | IGNCR |
                        INPCK | ISTRIP | IXON | IXOFF | IXANY);
    config.c_oflag = 0;
    config.c_lflag = 0;
    config.c_cc[VMIN] = 0;
    config.c_cc[VTIME] = 0;
    config.c_ispeed = LD2410_BAUD;
    config.c_ospeed = LD2410_BAUD;

    if (ioctl(fd, TCSETS2, &config) != 0) {
        perror("TCSETS2");
        return -1;
    }

    return 0;
}

static void print_hex(const uint8_t *data, size_t length, size_t *offset)
{
    for (size_t index = 0; index < length; ++index) {
        if ((*offset % 16U) == 0U) {
            printf("%08zx  ", *offset);
        }

        printf("%02x ", data[index]);
        ++(*offset);

        if ((*offset % 16U) == 0U) {
            putchar('\n');
        }
    }
}

int main(int argc, char *argv[])
{
    const char *device = (argc > 1) ? argv[1] : DEFAULT_DEVICE;
    int seconds = (argc > 2) ? atoi(argv[2]) : 5;
    struct pollfd poll_fd;
    uint8_t buffer[READ_BUFFER_SIZE];
    int fd;
    int64_t deadline;
    size_t offset = 0;
    int received_any = 0;

    if (seconds <= 0) {
        fprintf(stderr, "capture seconds must be positive\n");
        return EXIT_FAILURE;
    }

    fd = open(device, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        perror(device);
        return EXIT_FAILURE;
    }

    if (configure_uart_256000(fd) != 0) {
        close(fd);
        return EXIT_FAILURE;
    }

    deadline = monotonic_ms() + (int64_t)seconds * 1000;
    if (deadline < 0) {
        perror("clock_gettime");
        close(fd);
        return EXIT_FAILURE;
    }

    fprintf(stderr, "capturing %s at %u-8-N-1 for %d second(s)\n",
            device, LD2410_BAUD, seconds);

    poll_fd.fd = fd;
    poll_fd.events = POLLIN;
    poll_fd.revents = 0;

    while (monotonic_ms() < deadline) {
        int64_t remaining = deadline - monotonic_ms();
        int timeout_ms = (remaining > 200) ? 200 : (int)remaining;
        int poll_result = poll(&poll_fd, 1, timeout_ms);

        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }

            perror("poll");
            close(fd);
            return EXIT_FAILURE;
        }

        if (poll_result == 0 || (poll_fd.revents & POLLIN) == 0) {
            continue;
        }

        ssize_t bytes_read = read(fd, buffer, sizeof(buffer));
        if (bytes_read < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                continue;
            }

            perror("read");
            close(fd);
            return EXIT_FAILURE;
        }

        if (bytes_read > 0) {
            print_hex(buffer, (size_t)bytes_read, &offset);
            fflush(stdout);
            received_any = 1;
        }
    }

    if ((offset % 16U) != 0U) {
        putchar('\n');
    }

    close(fd);

    if (!received_any) {
        fprintf(stderr, "no UART bytes received\n");
        return 2;
    }

    return EXIT_SUCCESS;
}