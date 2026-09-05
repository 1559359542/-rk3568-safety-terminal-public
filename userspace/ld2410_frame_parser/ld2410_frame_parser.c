#include <asm/ioctls.h>
#include <asm/termbits.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_DEVICE "/dev/ttyS9"
#define LD2410_BAUD 256000U
#define READ_BUFFER_SIZE 256U
#define MAX_PAYLOAD_SIZE 64U
#define FRAME_HEADER_SIZE 4U
#define FRAME_LENGTH_SIZE 2U
#define FRAME_FOOTER_SIZE 4U
#define FRAME_BUFFER_SIZE \
    (FRAME_HEADER_SIZE + FRAME_LENGTH_SIZE + MAX_PAYLOAD_SIZE + FRAME_FOOTER_SIZE)

static const uint8_t report_header[FRAME_HEADER_SIZE] = {
    0xf4, 0xf3, 0xf2, 0xf1
};

static const uint8_t report_footer[FRAME_FOOTER_SIZE] = {
    0xf8, 0xf7, 0xf6, 0xf5
};

struct frame_assembler {
    uint8_t bytes[FRAME_BUFFER_SIZE];
    size_t used;
    size_t expected_total;
};

static int64_t monotonic_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return -1;
    }

    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static uint16_t read_u16_le(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
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

static const char *target_state_name(uint8_t state)
{
    switch (state) {
    case 0x00:
        return "none";
    case 0x01:
        return "moving";
    case 0x02:
        return "static";
    case 0x03:
        return "moving_and_static";
    default:
        return "unknown";
    }
}

static void assembler_reset(struct frame_assembler *assembler)
{
    assembler->used = 0;
    assembler->expected_total = 0;
}

/* 只接受协议定义的普通模式 13 字节载荷。 */
static bool is_normal_report_frame(const struct frame_assembler *assembler)
{
    const uint8_t *payload = &assembler->bytes[6];

    return assembler->expected_total == 23U &&
           payload[0] == 0x02 &&
           payload[1] == 0xaa &&
           payload[11] == 0x55 &&
           payload[12] == 0x00;
}

static bool has_valid_footer(const struct frame_assembler *assembler)
{
    const uint8_t *footer =
        &assembler->bytes[assembler->expected_total - FRAME_FOOTER_SIZE];

    return memcmp(footer, report_footer, FRAME_FOOTER_SIZE) == 0;
}

static void print_normal_report(const struct frame_assembler *assembler,
                                size_t frame_number)
{
    const uint8_t *payload = &assembler->bytes[6];
    uint8_t state = payload[2];
    uint16_t moving_distance_cm = read_u16_le(&payload[3]);
    uint8_t moving_energy = payload[5];
    uint16_t static_distance_cm = read_u16_le(&payload[6]);
    uint8_t static_energy = payload[8];
    uint16_t detection_distance_cm = read_u16_le(&payload[9]);

    printf("frame=%zu state=%s(0x%02x) "
           "moving_distance_cm=%u moving_energy=%u "
           "static_distance_cm=%u static_energy=%u "
           "detection_distance_cm=%u\n",
           frame_number,
           target_state_name(state),
           (unsigned)state,
           (unsigned)moving_distance_cm,
           (unsigned)moving_energy,
           (unsigned)static_distance_cm,
           (unsigned)static_energy,
           (unsigned)detection_distance_cm);
    fflush(stdout);
}

static void process_complete_frame(const struct frame_assembler *assembler,
                                   size_t *valid_frames)
{
    if (!has_valid_footer(assembler)) {
        fprintf(stderr, "discarded frame: invalid footer\n");
        return;
    }

    if (!is_normal_report_frame(assembler)) {
        fprintf(stderr, "ignored frame: unsupported payload format, length=%zu\n",
                assembler->expected_total - FRAME_HEADER_SIZE -
                FRAME_LENGTH_SIZE - FRAME_FOOTER_SIZE);
        return;
    }

    ++(*valid_frames);
    print_normal_report(assembler, *valid_frames);
}

/*
 * 串口 read() 的边界不等于协议帧边界：
 * 此状态机可处理半帧、多帧和帧头前的噪声字节。
 */
static void assembler_feed(struct frame_assembler *assembler,
                           uint8_t byte,
                           size_t *valid_frames)
{
    if (assembler->used < FRAME_HEADER_SIZE) {
        if (byte == report_header[assembler->used]) {
            assembler->bytes[assembler->used++] = byte;
        } else {
            assembler->used = 0;
            if (byte == report_header[0]) {
                assembler->bytes[assembler->used++] = byte;
            }
        }
        return;
    }

    assembler->bytes[assembler->used++] = byte;

    if (assembler->used == FRAME_HEADER_SIZE + FRAME_LENGTH_SIZE) {
        uint16_t payload_length = read_u16_le(&assembler->bytes[4]);

        if (payload_length > MAX_PAYLOAD_SIZE) {
            fprintf(stderr, "discarded frame: payload length=%u exceeds limit\n",
                    (unsigned)payload_length);
            assembler_reset(assembler);
            return;
        }

        assembler->expected_total = FRAME_HEADER_SIZE +
                                    FRAME_LENGTH_SIZE +
                                    payload_length +
                                    FRAME_FOOTER_SIZE;
    }

    if (assembler->expected_total != 0U &&
        assembler->used == assembler->expected_total) {
        process_complete_frame(assembler, valid_frames);
        assembler_reset(assembler);
    }
}

int main(int argc, char *argv[])
{
    const char *device = (argc > 1) ? argv[1] : DEFAULT_DEVICE;
    int seconds = (argc > 2) ? atoi(argv[2]) : 10;
    struct frame_assembler assembler;
    struct pollfd poll_fd;
    uint8_t read_buffer[READ_BUFFER_SIZE];
    int fd;
    int64_t deadline;
    size_t valid_frames = 0;

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

    memset(&assembler, 0, sizeof(assembler));
    poll_fd.fd = fd;
    poll_fd.events = POLLIN;

    fprintf(stderr, "parsing %s at %u-8-N-1 for %d second(s)\n",
            device, LD2410_BAUD, seconds);

    while (monotonic_ms() < deadline) {
        int64_t remaining = deadline - monotonic_ms();
        int timeout_ms = (remaining > 200) ? 200 : (int)remaining;
        int poll_result;

        poll_fd.revents = 0;
        poll_result = poll(&poll_fd, 1, timeout_ms);

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

        {
            ssize_t bytes_read = read(fd, read_buffer, sizeof(read_buffer));

            if (bytes_read < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK ||
                    errno == EINTR) {
                    continue;
                }

                perror("read");
                close(fd);
                return EXIT_FAILURE;
            }

            for (ssize_t index = 0; index < bytes_read; ++index) {
                assembler_feed(&assembler, read_buffer[index], &valid_frames);
            }
        }
    }

    close(fd);

    if (valid_frames == 0U) {
        fprintf(stderr, "no valid normal report frame received\n");
        return 2;
    }

    fprintf(stderr, "valid_normal_frames=%zu\n", valid_frames);
    return EXIT_SUCCESS;
}