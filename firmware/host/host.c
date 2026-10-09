/*
 * The microcontroller client's core on Linux, for testing without hardware:
 * the same client.c, with this file as the board layer. Audio is a WAV file
 * replayed in real time (as from the ICS-43434 on the left channel); Enter
 * stands in for the positioning button.
 *
 *   make -C firmware/host
 *   firmware/host/bps-host-client -s 192.168.1.10 -w birds.wav [-p 2473] [-n "Name"] [-i UUID]
 *
 * -S MS blocks every TCP write for MS ms every 15 s, to test Wi-Fi stalls.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "capture.h"
#include "client.h"
#include "port.h"

uint64_t port_mono_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
}

static uint8_t chip_id[8] = {'h', 'o', 's', 't', 0, 0, 0, 1};

void port_unique_id(uint8_t out[8]) { memcpy(out, chip_id, 8); }

size_t port_free_heap(void) { return 64 << 20; } /* plenty */

void port_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[%10lu] ", (unsigned long)(port_mono_us() / 1000));
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

void port_wifi_begin(const char *ssid, const char *password, const char *hostname)
{
    (void)ssid;
    (void)password;
    (void)hostname;
}

bool port_wifi_up(void) { return true; }

int port_wifi_rssi(void) { return 0; }

static bool resolve(const char *host, uint16_t port, int type, struct sockaddr_in *out)
{
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = type}, *res = NULL;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res)
        return false;
    memcpy(out, res->ai_addr, sizeof *out);
    out->sin_port = htons(port);
    freeaddrinfo(res);
    return true;
}

int port_tcp_connect(const char *host, uint16_t port)
{
    struct sockaddr_in addr;
    if (!resolve(host, port, SOCK_STREAM, &addr))
        return -1;
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0)
        return -1;
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    /* A small send buffer, like an MCU's, so slow networks show up as overruns. */
    int sndbuf = 16384;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof sndbuf);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }
    fd_set w;
    FD_ZERO(&w);
    FD_SET(fd, &w);
    struct timeval tv = {2, 0};
    int err = 0;
    socklen_t len = sizeof err;
    if (select(fd + 1, NULL, &w, NULL, &tv) != 1 ||
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err) {
        close(fd);
        return -1;
    }
    return fd;
}

/* -S: block every TCP write for this long every 15 s, like a Wi-Fi stall. */
static unsigned stall_ms;

int port_tcp_write(int h, const void *buf, size_t len)
{
    if (stall_ms && port_mono_us() / 1000 % 15000 < stall_ms)
        return 0;
    ssize_t n = send(h, buf, len, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (n >= 0)
        return (int)n;
    return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
}

int port_tcp_read(int h, void *buf, size_t len)
{
    ssize_t n = recv(h, buf, len, MSG_DONTWAIT);
    if (n > 0)
        return (int)n;
    if (n == 0)
        return -1;
    return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
}

void port_tcp_close(int h)
{
    if (h >= 0)
        close(h);
}

int port_udp_open(const char *host, uint16_t port)
{
    struct sockaddr_in addr;
    if (!resolve(host, port, SOCK_DGRAM, &addr))
        return -1;
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (fd >= 0 && connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int port_udp_send(int h, const void *buf, size_t len)
{
    return send(h, buf, len, MSG_DONTWAIT) == (ssize_t)len ? (int)len : -1;
}

int port_udp_recv(int h, void *buf, size_t len)
{
    ssize_t n = recv(h, buf, len, MSG_DONTWAIT);
    if (n >= 0)
        return (int)n;
    return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
}

void port_udp_close(int h)
{
    if (h >= 0)
        close(h);
}

/* ---- Audio: a WAV file replayed in real time, looping ---- */

static int16_t *wav;
static size_t wav_len;
static uint32_t wav_rate;

static bool load_wav(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    uint8_t h[12];
    bool ok = fread(h, 1, 12, f) == 12 && !memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVE", 4);
    uint16_t channels = 0, bits = 0, fmt = 0;
    while (ok) {
        uint8_t c[8];
        if (fread(c, 1, 8, f) != 8)
            break;
        uint32_t size = c[4] | c[5] << 8 | c[6] << 16 | (uint32_t)c[7] << 24;
        if (!memcmp(c, "fmt ", 4)) {
            uint8_t b[16];
            if (size < 16 || fread(b, 1, 16, f) != 16)
                break;
            fmt = b[0] | b[1] << 8;
            channels = b[2] | b[3] << 8;
            wav_rate = b[4] | b[5] << 8 | b[6] << 16 | (uint32_t)b[7] << 24;
            bits = b[14] | b[15] << 8;
            fseek(f, (long)size - 16 + (size & 1), SEEK_CUR);
        } else if (!memcmp(c, "data", 4)) {
            if (fmt != 1 || bits != 16 || channels == 0)
                break;
            size_t frames = size / (2u * channels);
            int16_t *all = malloc(size);
            wav = malloc(frames * sizeof *wav);
            if (!all || !wav || fread(all, 2 * channels, frames, f) != frames)
                break;
            for (size_t i = 0; i < frames; i++)
                wav[i] = all[i * channels]; /* first channel */
            free(all);
            wav_len = frames;
            fclose(f);
            return true;
        } else {
            fseek(f, (long)size + (size & 1), SEEK_CUR);
        }
    }
    fclose(f);
    return false;
}

static void *audio_thread(void *arg)
{
    uint32_t rate = (uint32_t)(uintptr_t)arg;
    enum { BLOCK = 256 };
    static int32_t lr[2 * BLOCK];
    uint64_t start = port_mono_us(), frames = 0;
    size_t pos = 0;
    for (;;) {
        for (int i = 0; i < BLOCK; i++) {
            lr[2 * i] = (int32_t)wav[pos] << 16; /* left: the mic, 24-bit left-aligned */
            lr[2 * i + 1] = 0;
            pos = (pos + 1) % wav_len;
        }
        frames += BLOCK;
        /* Deliver once the block has "finished recording", a little late. */
        uint64_t due = start + frames * 1000000 / rate + (uint64_t)(rand() % 2000);
        uint64_t now = port_mono_us();
        if (due > now)
            usleep((useconds_t)(due - now));
        bps_capture_frames(lr, BLOCK, port_mono_us());
    }
    return NULL;
}

void port_audio_start(uint32_t sample_rate)
{
    if (sample_rate != wav_rate)
        port_log("audio: the WAV is %u Hz but the client runs at %u Hz; it will sound off",
                 (unsigned)wav_rate, (unsigned)sample_rate);
    pthread_t t;
    pthread_create(&t, NULL, audio_thread, (void *)(uintptr_t)sample_rate);
}

void port_led(bool on) { (void)on; }

static volatile uint64_t enter_until;

static void *stdin_thread(void *arg)
{
    (void)arg;
    int c;
    while ((c = getchar()) != EOF)
        if (c == '\n')
            enter_until = port_mono_us() + 200000;
    return NULL;
}

bool port_button(void) { return port_mono_us() < enter_until; }

int main(int argc, char **argv)
{
    bps_settings s = {
        .wifi_ssid = "host",
        .wifi_password = "",
        .server_host = "127.0.0.1",
        .server_port = 2473,
        .name = "",
        .client_id = "",
        .device_name = "WAV replay (host build)",
        .board = "host",
        .version = "bps-mcu 0.1.0 (host)",
        .sample_rate = 48000,
        .mic_channel = 0,
        .ring_bytes = 160 * 1024,
        .lookback_ms = 1000,
    };
    const char *wav_path = NULL;
    int opt;
    while ((opt = getopt(argc, argv, "s:p:n:i:w:r:l:S:")) != -1) {
        switch (opt) {
        case 's': s.server_host = optarg; break;
        case 'p': s.server_port = (uint16_t)atoi(optarg); break;
        case 'n': s.name = optarg; break;
        case 'i': s.client_id = optarg; break;
        case 'w': wav_path = optarg; break;
        case 'r': s.ring_bytes = (uint32_t)atoi(optarg) * 1024; break;
        case 'l': s.lookback_ms = (uint32_t)atoi(optarg); break;
        case 'S': stall_ms = (unsigned)atoi(optarg); break;
        default:
            fprintf(stderr, "usage: %s -s SERVER -w FILE.wav [-p PORT] [-n NAME] [-i UUID] "
                            "[-r RING_KB] [-l LOOKBACK_MS] [-S STALL_MS]\n", argv[0]);
            return 2;
        }
    }
    if (!wav_path || !load_wav(wav_path)) {
        fprintf(stderr, "need -w with a 16-bit PCM WAV file\n");
        return 2;
    }
    pthread_t t;
    pthread_create(&t, NULL, stdin_thread, NULL);
    bps_client_setup(&s);
    for (;;) {
        bps_client_loop();
        usleep(500); /* an MCU loop spins; this keeps the host idle */
    }
}
