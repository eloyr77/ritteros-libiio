/*
 * The iiod client: libiio's network backend, our own code from
 * iiod's text protocol (version 0.x, TCP port 30431). A command is one line
 * ending in "\r\n"; the answer begins with a line holding a number: a
 * negative error (Linux's numbers, which are RitterOS's too), or a count.
 *
 *     VERSION                          "0.26.0000000\n" (major.minor.git tag)
 *     PRINT                            length, the XML, "\n"
 *     TIMEOUT ms                       0
 *     READ dev [INPUT|OUTPUT chn|DEBUG|BUFFER] attr
 *                                      length, the value, "\n"
 *     WRITE dev [...] attr length      then the bytes; the bytes taken
 *     GETTRIG dev / SETTRIG dev [trig] length and the trigger's name / 0
 *     SET dev BUFFERS_COUNT n          0
 *     OPEN dev samples mask [CYCLIC]   0 (the mask as 8-hex-digit words,
 *                                      the highest first)
 *     READBUF dev bytes                pieces: length, the mask (once,
 *                                      "\n" after it), the bytes; a 0
 *                                      length ends the answer
 *     WRITEBUF dev bytes               0, then the bytes, then the count
 *     CLOSE dev                        0
 *
 * Attributes go over the context's connection; each buffer has a
 * connection of its own (iiod streams on it until CLOSE), as libiio does.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "ritteros.h"
#include "iiopriv.h"

/* Waits on the server are cut into slices this long, so that a cancelled
 * buffer notices (iio_buffer_cancel). */
#define SLICE_MS 100

void iio_lock_take(struct iio_lock *l)
{
    unsigned me = __atomic_fetch_add(&l->next, 1, __ATOMIC_ACQUIRE);
    while (__atomic_load_n(&l->serving, __ATOMIC_ACQUIRE) != me)
        usleep(1000);
}

void iio_lock_give(struct iio_lock *l)
{
    __atomic_fetch_add(&l->serving, 1, __ATOMIC_RELEASE);
}

int iio_net_connect(uint32_t ip, uint16_t port, unsigned timeout_ms)
{
    (void)timeout_ms;                   /* (connect has the kernel's own limit) */
    int fd = ritteros_socket(SOCKET_TCP);
    if (fd < 0)
        return -errno;
    if (ritteros_connect(fd, ip, port) < 0) {
        int err = errno;
        close(fd);
        return -err;
    }
    return fd;
}

int iio_net_write_all(int fd, const void *src, size_t len)
{
    const char *p = src;
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            return -errno;
        if (n == 0)
            return -EPIPE;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

/* Up to len bytes, waiting at most timeout_ms (0: for ever) for the
 * first; *cancel (if given) stops a wait. The count, or a negative error
 * (-EPIPE: the server closed the connection). */
static ssize_t receive(int fd, unsigned timeout_ms, void *dst, size_t len, const bool *cancel)
{
    unsigned waited = 0;
    for (;;) {
        if (cancel && __atomic_load_n(cancel, __ATOMIC_ACQUIRE))
            return -EBADF;
        unsigned slice = SLICE_MS;
        if (timeout_ms && timeout_ms - waited < slice)
            slice = timeout_ms - waited;
        ssize_t n = ritteros_recvfrom(fd, dst, len, 0, 0, slice);
        if (n > 0)
            return n;
        if (n == 0)
            return -EPIPE;
        if (errno == EINTR)
            continue;
        if (errno != ETIMEDOUT && errno != EAGAIN)
            return -errno;
        waited += slice;
        if (timeout_ms && waited >= timeout_ms)
            return -ETIMEDOUT;
    }
}

static int read_all_cancel(int fd, unsigned timeout_ms, void *dst, size_t len, const bool *cancel)
{
    char *p = dst;
    while (len) {
        ssize_t n = receive(fd, timeout_ms, p, len, cancel);
        if (n < 0)
            return (int)n;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

int iio_net_read_all(int fd, unsigned timeout_ms, void *dst, size_t len)
{
    return read_all_cancel(fd, timeout_ms, dst, len, 0);
}

/* A line (without its "\n"; a "\r" before it dropped) into buf. */
static int read_line(int fd, unsigned timeout_ms, char *buf, size_t size, const bool *cancel)
{
    size_t n = 0;
    for (;;) {
        char c;
        int ret = read_all_cancel(fd, timeout_ms, &c, 1, cancel);
        if (ret < 0)
            return ret;
        if (c == '\n')
            break;
        if (n + 1 >= size)
            return -EIO;                /* (no answer line is this long) */
        buf[n++] = c;
    }
    if (n && buf[n - 1] == '\r')
        n--;
    buf[n] = 0;
    return (int)n;
}

/* The number that begins an answer (empty lines are skipped, as iiod may
 * send one). */
static int read_integer(int fd, unsigned timeout_ms, int *value, const bool *cancel)
{
    char line[64];
    for (;;) {
        int n = read_line(fd, timeout_ms, line, sizeof(line), cancel);
        if (n < 0)
            return n;
        if (n == 0)
            continue;
        char *end;
        long v = strtol(line, &end, 10);
        if (end == line || v < -0x7FFFFFFFL || v > 0x7FFFFFFFL)
            return -EIO;
        *value = (int)v;
        return 0;
    }
}

int iio_net_integer(int fd, unsigned timeout_ms, int *value)
{
    return read_integer(fd, timeout_ms, value, 0);
}

int iio_net_command(int fd, unsigned timeout_ms, const char *cmd)
{
    int ret = iio_net_write_all(fd, cmd, strlen(cmd));
    if (ret < 0)
        return ret;
    int value;
    ret = read_integer(fd, timeout_ms, &value, 0);
    return ret < 0 ? ret : value;
}

/* The part of a READ or WRITE command after the device: which kind of
 * attribute, and its name. */
static void attr_target(char *out, size_t size, const struct iio_device *dev,
                        const struct iio_channel *chn, enum iio_attr_kind kind, const char *attr)
{
    if (kind == ATTR_CHANNEL)
        snprintf(out, size, "%s %s %s %s", dev->id, chn->output ? "OUTPUT" : "INPUT", chn->id,
                 attr);
    else if (kind == ATTR_DEBUG)
        snprintf(out, size, "%s DEBUG %s", dev->id, attr);
    else if (kind == ATTR_BUFFER)
        snprintf(out, size, "%s BUFFER %s", dev->id, attr);
    else
        snprintf(out, size, "%s %s", dev->id, attr);
}

/* Throw away n bytes of an answer that does not fit, so that the
 * connection stays in step. */
static int discard(int fd, unsigned timeout_ms, size_t n)
{
    char sink[256];
    while (n) {
        size_t k = n < sizeof(sink) ? n : sizeof(sink);
        int ret = iio_net_read_all(fd, timeout_ms, sink, k);
        if (ret < 0)
            return ret;
        n -= k;
    }
    return 0;
}

ssize_t iio_net_attr_read(const struct iio_device *dev, const struct iio_channel *chn,
                          enum iio_attr_kind kind, const char *attr, char *dst, size_t len)
{
    struct iio_context *ctx = dev->ctx;
    char cmd[512], target[480];
    if (!ctx->network)
        return -ENOSYS;
    if (len == 0)
        return -EINVAL;
    attr_target(target, sizeof(target), dev, chn, kind, attr);
    snprintf(cmd, sizeof(cmd), "READ %s\r\n", target);
    iio_lock_take(&ctx->lock);
    ssize_t ret = iio_net_command(ctx->fd, ctx->timeout_ms, cmd);
    if (ret >= 0) {
        size_t n = (size_t)ret;
        if (n + 1 > len) {                  /* the value and its "\n" */
            int d = discard(ctx->fd, ctx->timeout_ms, n + 1);
            ret = d < 0 ? d : -EIO;
        } else {
            int r = iio_net_read_all(ctx->fd, ctx->timeout_ms, dst, n + 1);
            if (r < 0) {
                ret = r;
            } else {
                dst[n] = 0;                 /* (over the "\n") */
                ret = (ssize_t)n;
            }
        }
    }
    iio_lock_give(&ctx->lock);
    return ret;
}

ssize_t iio_net_attr_write(const struct iio_device *dev, const struct iio_channel *chn,
                           enum iio_attr_kind kind, const char *attr, const void *src,
                           size_t len)
{
    struct iio_context *ctx = dev->ctx;
    char cmd[512], target[480];
    if (!ctx->network)
        return -ENOSYS;
    attr_target(target, sizeof(target), dev, chn, kind, attr);
    snprintf(cmd, sizeof(cmd), "WRITE %s %lu\r\n", target, (unsigned long)len);
    iio_lock_take(&ctx->lock);
    int ret = iio_net_write_all(ctx->fd, cmd, strlen(cmd));
    if (ret == 0)
        ret = iio_net_write_all(ctx->fd, src, len);
    int value = 0;
    if (ret == 0)
        ret = read_integer(ctx->fd, ctx->timeout_ms, &value, 0);
    iio_lock_give(&ctx->lock);
    return ret < 0 ? ret : value;
}

/* --- Buffers --- */

static int words_of(const struct iio_device *dev)
{
    return (int)dev->words;
}

int iio_net_open(struct iio_buffer *buf)
{
    const struct iio_device *dev = buf->dev;
    struct iio_context *ctx = dev->ctx;
    if (!ctx->network)
        return -ENOSYS;
    int fd = iio_net_connect(ctx->ip, ctx->port, ctx->timeout_ms);
    if (fd < 0)
        return fd;
    char cmd[640];
    int n = snprintf(cmd, sizeof(cmd), "OPEN %s %lu ", dev->id, (unsigned long)buf->samples);
    for (int i = words_of(dev); i > 0; i--)
        n += snprintf(cmd + n, sizeof(cmd) - (size_t)n, "%08x", (unsigned)buf->mask[i - 1]);
    snprintf(cmd + n, sizeof(cmd) - (size_t)n, buf->cyclic ? " CYCLIC\r\n" : "\r\n");
    int ret = iio_net_command(fd, ctx->timeout_ms, cmd);
    if (ret < 0) {
        close(fd);
        return ret;
    }
    buf->fd = fd;
    return 0;
}

void iio_net_close(struct iio_buffer *buf)
{
    if (buf->fd < 0)
        return;
    if (!buf->cancelled) {
        char cmd[320];
        snprintf(cmd, sizeof(cmd), "CLOSE %s\r\n", buf->dev->id);
        iio_net_command(buf->fd, buf->dev->ctx->timeout_ms, cmd);
        iio_net_write_all(buf->fd, "\r\nEXIT\r\n", 8);
    }
    close(buf->fd);
    buf->fd = -1;
}

/* The mask line of a READBUF answer: words 8-hex-digit groups, highest
 * word first, then "\n". */
static int read_mask(struct iio_buffer *buf, unsigned timeout_ms)
{
    int words = words_of(buf->dev);
    char text[8 * 64 + 1];
    if (words > 64)
        return -EIO;
    int ret = read_all_cancel(buf->fd, timeout_ms, text, (size_t)words * 8 + 1, &buf->cancelled);
    if (ret < 0)
        return ret;
    for (int i = 0; i < words; i++) {
        uint32_t v = 0;
        for (int k = 0; k < 8; k++) {
            char c = text[i * 8 + k];
            int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
                  : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (d < 0)
                return -EIO;
            v = v << 4 | (uint32_t)d;
        }
        buf->mask[words - 1 - i] = v;
    }
    return 0;
}

ssize_t iio_net_readbuf(struct iio_buffer *buf, void *dst, size_t len)
{
    unsigned timeout = buf->dev->ctx->timeout_ms;
    char cmd[320];
    snprintf(cmd, sizeof(cmd), "READBUF %s %lu\r\n", buf->dev->id, (unsigned long)len);
    int ret = iio_net_write_all(buf->fd, cmd, strlen(cmd));
    if (ret < 0)
        return ret;
    size_t got = 0;
    bool mask_read = false;
    while (got < len) {
        int n;
        ret = read_integer(buf->fd, timeout, &n, &buf->cancelled);
        if (ret < 0)
            return ret;
        if (n < 0)
            return n;
        if (n == 0)
            break;
        if (!mask_read) {
            ret = read_mask(buf, timeout);
            if (ret < 0)
                return ret;
            mask_read = true;
        }
        if ((size_t)n > len - got)
            return -EIO;                    /* (more than asked for) */
        ret = read_all_cancel(buf->fd, timeout, (char *)dst + got, (size_t)n, &buf->cancelled);
        if (ret < 0)
            return ret;
        got += (size_t)n;
    }
    return (ssize_t)got;
}

ssize_t iio_net_writebuf(struct iio_buffer *buf, const void *src, size_t len)
{
    unsigned timeout = buf->dev->ctx->timeout_ms;
    char cmd[320];
    snprintf(cmd, sizeof(cmd), "WRITEBUF %s %lu\r\n", buf->dev->id, (unsigned long)len);
    int ret = iio_net_write_all(buf->fd, cmd, strlen(cmd));
    int value;
    if (ret == 0)
        ret = read_integer(buf->fd, timeout, &value, &buf->cancelled);
    if (ret < 0)
        return ret;
    if (value < 0)
        return value;
    ret = iio_net_write_all(buf->fd, src, len);
    if (ret == 0)
        ret = read_integer(buf->fd, timeout, &value, &buf->cancelled);
    if (ret < 0)
        return ret;
    return value < 0 ? value : (ssize_t)len;
}
