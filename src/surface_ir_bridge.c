// Surface Pro OV7251/IPU3 bridge for V4L2 consumers such as Gaze.
// SPDX-License-Identifier: MIT

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <linux/videodev2.h>

#define WIDTH 640
#define HEIGHT 480
#define INPUT_FOURCC v4l2_fourcc('i', 'p', '3', 'y')
#define DEFAULT_INPUT "/dev/video2"
#define DEFAULT_OUTPUT "/dev/video42"
#define RUNTIME_INPUT "/run/surface_ir_bridge_dev"
#define BUFFER_COUNT 4
#define PACKED_STRIDE (((WIDTH + 24) / 25) * 32)

static volatile sig_atomic_t running = 1;
static volatile sig_atomic_t stats_requested = 0;

struct bridge_stats {
    uint64_t input_frames;
    uint64_t output_frames;
    uint64_t filtered_frames;
    uint64_t sequence_anomalies;
    uint64_t estimated_missing;
    uint64_t flagged_errors;
    uint32_t last_sequence;
    struct timeval last_timestamp;
    int has_last_sequence;
};

struct mapped_buffer {
    void *addr;
    size_t length;
};

static void stop_handler(int signal_number) {
    (void)signal_number;
    running = 0;
}

static void stats_handler(int signal_number) {
    (void)signal_number;
    stats_requested = 1;
}

static void print_stats(const struct bridge_stats *stats) {
    fprintf(stderr,
            "surface-ir-bridge: stats input=%" PRIu64 " output=%" PRIu64
            " filtered=%" PRIu64 " sequence_anomalies=%" PRIu64
            " estimated_missing=%" PRIu64 " flagged_errors=%" PRIu64
            " last_sequence=%u last_timestamp=%lld.%06ld\n",
            stats->input_frames, stats->output_frames, stats->filtered_frames,
            stats->sequence_anomalies, stats->estimated_missing,
            stats->flagged_errors, stats->last_sequence,
            (long long)stats->last_timestamp.tv_sec,
            (long)stats->last_timestamp.tv_usec);
}

static void usage(const char *program) {
    fprintf(stderr,
            "Usage: %s [--input PATH] [--output PATH] [--min-brightness N]\n"
            "       %s [--debug]\n\n"
            "Reads IPU3 ip3y (10-bit packed) and writes GREY frames.\n"
            "Send SIGUSR1 to print cumulative source-frame statistics.\n",
            program, program);
}

static int open_v4l2(const char *path, unsigned required_caps, int flags) {
    int fd = open(path, flags | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "surface-ir-bridge: open %s: %s\n", path,
                strerror(errno));
        return -1;
    }
    struct v4l2_capability cap = {0};
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
        fprintf(stderr, "surface-ir-bridge: %s is not V4L2: %s\n", path,
                strerror(errno));
        close(fd);
        return -1;
    }
    unsigned caps = cap.capabilities;
    if (caps & V4L2_CAP_DEVICE_CAPS)
        caps = cap.device_caps;
    if ((caps & required_caps) != required_caps) {
        fprintf(stderr, "surface-ir-bridge: %s lacks required V4L2 caps\n",
                path);
        close(fd);
        return -1;
    }
    return fd;
}

static const char *runtime_input(void) {
    static char path[128];
    FILE *file = fopen(RUNTIME_INPUT, "r");
    if (!file)
        return DEFAULT_INPUT;
    if (!fgets(path, sizeof(path), file)) {
        fclose(file);
        return DEFAULT_INPUT;
    }
    fclose(file);
    path[strcspn(path, "\r\n")] = '\0';
    return path[0] ? path : DEFAULT_INPUT;
}

/* ip3y stores 25 ten-bit pixels in each 32-byte group, with six padding
 * bits in the last byte. Do not treat a whole scanline as one bitstream. */
static void unpack_line(const uint8_t *packed, uint8_t *grey, size_t stride) {
    for (unsigned x = 0; x < WIDTH; ++x) {
        unsigned group = x / 25;
        unsigned in_group = x % 25;
        size_t base = (size_t)group * 32;
        if (base + 31 >= stride)
            break;
        if (in_group < 24) {
            unsigned quartet = in_group / 4;
            unsigned pixel = in_group % 4;
            const uint8_t *b = packed + base + quartet * 5;
            uint16_t value;
            switch (pixel) {
            case 0: value = (uint16_t)b[0] | ((b[1] & 0x03u) << 8); break;
            case 1: value = (uint16_t)(b[1] >> 2) | ((b[2] & 0x0fu) << 6); break;
            case 2: value = (uint16_t)(b[2] >> 4) | ((b[3] & 0x3fu) << 4); break;
            default: value = (uint16_t)(b[3] >> 6) | ((uint16_t)b[4] << 2); break;
            }
            grey[x] = (uint8_t)(value >> 2);
        } else {
            const uint8_t *b = packed + base + 30;
            grey[x] = (uint8_t)((b[0] | ((b[1] & 0x03u) << 8)) >> 2);
        }
    }
}

static int frame_brightness(const uint8_t *frame, size_t size) {
    uint64_t sum = 0;
    size_t count = 0;
    for (size_t i = 0; i < size; i += 64) {
        sum += frame[i];
        ++count;
    }
    return count ? (int)(sum / count) : 0;
}

static int write_full(int fd, const uint8_t *data, size_t size) {
    size_t written = 0;
    while (written < size) {
        ssize_t result = write(fd, data + written, size - written);
        if (result < 0 && errno == EINTR)
            continue;
        if (result <= 0)
            return -1;
        written += (size_t)result;
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *input = NULL;
    const char *output = DEFAULT_OUTPUT;
    int min_brightness = 0;
    int debug = 0;
    static const struct option options[] = {
        {"input", required_argument, NULL, 'i'},
        {"output", required_argument, NULL, 'o'},
        {"min-brightness", required_argument, NULL, 'b'},
        {"debug", no_argument, NULL, 'd'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };

    int option;
    while ((option = getopt_long(argc, argv, "i:o:b:dh", options, NULL)) != -1) {
        switch (option) {
        case 'i': input = optarg; break;
        case 'o': output = optarg; break;
        case 'b': min_brightness = atoi(optarg); break;
        case 'd': debug = 1; break;
        case 'h': usage(argv[0]); return 0;
        default: usage(argv[0]); return 2;
        }
    }
    if (min_brightness < 0 || min_brightness > 255) {
        fprintf(stderr, "surface-ir-bridge: min brightness must be 0..255\n");
        return 2;
    }
    if (!input)
        input = runtime_input();

    signal(SIGINT, stop_handler);
    signal(SIGTERM, stop_handler);
    signal(SIGUSR1, stats_handler);

    int in_fd = open_v4l2(input, V4L2_CAP_VIDEO_CAPTURE_MPLANE | V4L2_CAP_STREAMING,
                          O_RDWR);
    if (in_fd < 0)
        return 1;
    int out_fd = open_v4l2(output, V4L2_CAP_VIDEO_OUTPUT, O_WRONLY);
    if (out_fd < 0) {
        close(in_fd);
        return 1;
    }

    struct v4l2_format input_format = {0};
    input_format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    input_format.fmt.pix_mp.width = WIDTH;
    input_format.fmt.pix_mp.height = HEIGHT;
    input_format.fmt.pix_mp.pixelformat = INPUT_FOURCC;
    input_format.fmt.pix_mp.field = V4L2_FIELD_NONE;
    if (ioctl(in_fd, VIDIOC_S_FMT, &input_format) < 0) {
        perror("surface-ir-bridge: input S_FMT");
        goto fail;
    }
    if (input_format.fmt.pix_mp.num_planes != 1 ||
        input_format.fmt.pix_mp.width != WIDTH ||
        input_format.fmt.pix_mp.height != HEIGHT ||
        input_format.fmt.pix_mp.pixelformat != INPUT_FOURCC) {
        fprintf(stderr, "surface-ir-bridge: input did not accept 640x480 ip3y\n");
        goto fail;
    }
    size_t stride = input_format.fmt.pix_mp.plane_fmt[0].bytesperline;
    if (stride < PACKED_STRIDE || stride > SIZE_MAX / HEIGHT) {
        fprintf(stderr, "surface-ir-bridge: invalid ip3y stride: %zu\n", stride);
        goto fail;
    }
    size_t packed_frame_size = stride * HEIGHT;

    struct v4l2_format output_format = {0};
    output_format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    output_format.fmt.pix.width = WIDTH;
    output_format.fmt.pix.height = HEIGHT;
    output_format.fmt.pix.pixelformat = V4L2_PIX_FMT_GREY;
    output_format.fmt.pix.field = V4L2_FIELD_NONE;
    output_format.fmt.pix.bytesperline = WIDTH;
    output_format.fmt.pix.sizeimage = WIDTH * HEIGHT;
    if (ioctl(out_fd, VIDIOC_S_FMT, &output_format) < 0) {
        perror("surface-ir-bridge: output S_FMT");
        goto fail;
    }
    if (output_format.fmt.pix.pixelformat != V4L2_PIX_FMT_GREY ||
        output_format.fmt.pix.width != WIDTH ||
        output_format.fmt.pix.height != HEIGHT) {
        fprintf(stderr, "surface-ir-bridge: output does not support 640x480 GREY\n");
        goto fail;
    }

    struct v4l2_requestbuffers request = {0};
    request.count = BUFFER_COUNT;
    request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    request.memory = V4L2_MEMORY_MMAP;
    if (ioctl(in_fd, VIDIOC_REQBUFS, &request) < 0) {
        perror("surface-ir-bridge: input REQBUFS");
        goto fail;
    }
    if (request.count < 2 || request.count > BUFFER_COUNT) {
        fprintf(stderr, "surface-ir-bridge: unexpected input buffer count: %u\n",
                request.count);
        goto fail;
    }
    struct mapped_buffer buffers[BUFFER_COUNT] = {0};
    for (unsigned i = 0; i < request.count; ++i) {
        struct v4l2_buffer buffer = {0};
        struct v4l2_plane plane = {0};
        buffer.type = request.type;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = i;
        buffer.length = 1;
        buffer.m.planes = &plane;
        if (ioctl(in_fd, VIDIOC_QUERYBUF, &buffer) < 0)
            goto fail_buffers;
        if (plane.length < packed_frame_size) {
            fprintf(stderr, "surface-ir-bridge: input buffer %u is too small\n", i);
            goto fail_buffers;
        }
        buffers[i].length = plane.length;
        buffers[i].addr = mmap(NULL, plane.length, PROT_READ | PROT_WRITE,
                               MAP_SHARED, in_fd, plane.m.mem_offset);
        if (buffers[i].addr == MAP_FAILED)
            goto fail_buffers;
        if (ioctl(in_fd, VIDIOC_QBUF, &buffer) < 0)
            goto fail_buffers;
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (ioctl(in_fd, VIDIOC_STREAMON, &type) < 0) {
        perror("surface-ir-bridge: STREAMON");
        goto fail_buffers;
    }
    uint8_t *frame = malloc(WIDTH * HEIGHT);
    if (!frame) {
        perror("surface-ir-bridge: frame allocation");
        goto fail_stream;
    }
    fprintf(stderr, "surface-ir-bridge: %s -> %s, stride=%zu%s\n", input,
            output, stride, debug ? " (debug)" : "");

    int status = 0;
    struct bridge_stats stats = {0};
    while (running) {
        if (stats_requested) {
            stats_requested = 0;
            print_stats(&stats);
        }
        struct pollfd pollfd = {.fd = in_fd, .events = POLLIN};
        int ready = poll(&pollfd, 1, 3000);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready <= 0) {
            if (ready < 0)
                perror("surface-ir-bridge: poll");
            else
                fprintf(stderr, "surface-ir-bridge: input timeout\n");
            status = 1;
            break;
        }

        struct v4l2_buffer buffer = {0};
        struct v4l2_plane plane = {0};
        buffer.type = type;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.length = 1;
        buffer.m.planes = &plane;
        if (ioctl(in_fd, VIDIOC_DQBUF, &buffer) < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            perror("surface-ir-bridge: DQBUF");
            status = 1;
            break;
        }
        if (buffer.index >= request.count ||
            plane.data_offset > plane.bytesused ||
            plane.bytesused - plane.data_offset < packed_frame_size ||
            plane.bytesused > buffers[buffer.index].length) {
            fprintf(stderr, "surface-ir-bridge: incomplete input frame\n");
            status = 1;
            break;
        }
        ++stats.input_frames;
        if (stats.has_last_sequence) {
            uint32_t expected = stats.last_sequence + 1;
            if (buffer.sequence != expected) {
                ++stats.sequence_anomalies;
                uint32_t delta = buffer.sequence - expected;
                if (delta < 0x80000000u)
                    stats.estimated_missing += delta;
            }
        }
        stats.last_sequence = buffer.sequence;
        stats.last_timestamp = buffer.timestamp;
        stats.has_last_sequence = 1;
        if (buffer.flags & V4L2_BUF_FLAG_ERROR)
            ++stats.flagged_errors;
        const uint8_t *packed = (const uint8_t *)buffers[buffer.index].addr +
                                plane.data_offset;
        for (unsigned y = 0; y < HEIGHT; ++y)
            unpack_line(packed + y * stride, frame + y * WIDTH, stride);
        int brightness = frame_brightness(frame, WIDTH * HEIGHT);
        if (debug)
            fprintf(stderr,
                    "surface-ir-bridge: frame=%u timestamp=%lld.%06ld flags=0x%08x brightness=%d\n",
                    buffer.sequence, (long long)buffer.timestamp.tv_sec,
                    (long)buffer.timestamp.tv_usec, buffer.flags, brightness);
        if (brightness >= min_brightness) {
            if (write_full(out_fd, frame, WIDTH * HEIGHT) < 0) {
                perror("surface-ir-bridge: loopback write");
                status = 1;
                break;
            }
            ++stats.output_frames;
        } else {
            ++stats.filtered_frames;
        }
        if (ioctl(in_fd, VIDIOC_QBUF, &buffer) < 0) {
            perror("surface-ir-bridge: QBUF");
            status = 1;
            break;
        }
    }
    if (debug || stats_requested)
        print_stats(&stats);
    free(frame);
    ioctl(in_fd, VIDIOC_STREAMOFF, &type);
    for (unsigned i = 0; i < request.count; ++i)
        munmap(buffers[i].addr, buffers[i].length);
    close(out_fd);
    close(in_fd);
    return status;

fail_stream:
    ioctl(in_fd, VIDIOC_STREAMOFF, &type);
fail_buffers:
    for (unsigned i = 0; i < BUFFER_COUNT; ++i)
        if (buffers[i].addr && buffers[i].addr != MAP_FAILED)
            munmap(buffers[i].addr, buffers[i].length);
fail:
    close(out_fd);
    close(in_fd);
    return 1;
}
