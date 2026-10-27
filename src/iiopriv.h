/*
 * libiio's insides, shared by iio.c (the objects and the
 * interface), iioxml.c (a context from its XML description) and iionet.c
 * (the iiod client: attributes and buffers over TCP). Not exported.
 */
#ifndef RITTEROS_IIOPRIV_H
#define RITTEROS_IIOPRIV_H

#include <errno.h>
#include <iio.h>
#include <pthread.h>
#include <stdint.h>

/* A lock of the connection: a pthread mutex (RitterOS's wait in the
 * kernel's futex, since its step 54a; the ticket lock this was before
 * slept a 10 ms tick between looks). Made and ended with the context. */
struct iio_lock {
    pthread_mutex_t mutex;
};
void iio_lock_take(struct iio_lock *l);
void iio_lock_give(struct iio_lock *l);

enum iio_attr_kind { ATTR_DEVICE, ATTR_BUFFER, ATTR_DEBUG, ATTR_CHANNEL };

struct iio_attr {
    char *name;
    char *filename;                 /* channel attributes: the sysfs file */
};

struct iio_channel {
    struct iio_device *dev;
    char *id, *name;
    bool output, scan_element, enabled;
    long index;
    struct iio_data_format format;
    enum iio_chan_type type;
    enum iio_modifier modifier;
    struct iio_attr *attrs;
    unsigned nattrs;
    unsigned number;                /* its place in the device's list */
    void *userdata;
};

struct iio_device {
    struct iio_context *ctx;
    char *id, *name, *label;
    struct iio_channel **channels;
    unsigned nchannels;
    struct iio_attr *attrs, *buffer_attrs, *debug_attrs;
    unsigned nattrs, nbuffer_attrs, ndebug_attrs;
    uint32_t *mask;                 /* enabled scan elements, by channel number */
    unsigned words;
    struct iio_buffer *buffer;      /* the one open buffer */
    void *userdata;
};

struct iio_context {
    char *name;                     /* "network" or "xml" */
    char *description;
    char *xml;
    char **attr_names, **attr_values;
    unsigned nattrs;
    struct iio_device **devices;
    unsigned ndevices;
    /* the server (network contexts) */
    bool network;
    char *host;                     /* as given, for clone */
    uint32_t ip;
    uint16_t port;
    int fd;                         /* -1: dropped, opened again by the next exchange */
    unsigned timeout_ms;
    bool timeout_sent;              /* iio_context_set_timeout told the server (per connection) */
    unsigned major, minor;
    char git_tag[8];
    struct iio_lock lock;
};

struct iio_buffer {
    const struct iio_device *dev;
    int fd;                         /* its own connection */
    bool cyclic, output, pushed, cancelled;
    size_t samples, sample_size, length, data_length;   /* length: allocated */
    char *data;
    uint32_t *mask;                 /* the channels the server sends */
    void *userdata;
};

/* Building a context (iioxml.c). */
struct iio_context *iio_context_new(const char *name);
int iio_context_add_attr(struct iio_context *ctx, const char *name, const char *value);
int iio_xml_parse(struct iio_context *ctx, const char *xml, size_t len);
void iio_channel_finish(struct iio_channel *chn);

/* The server (iionet.c). */
int iio_net_connect(uint32_t ip, uint16_t port, unsigned timeout_ms);
int iio_net_command(int fd, unsigned timeout_ms, const char *cmd);
int iio_net_read_all(int fd, unsigned timeout_ms, void *dst, size_t len);
int iio_net_write_all(int fd, const void *src, size_t len);
int iio_net_integer(int fd, unsigned timeout_ms, int *value);
/* Exchanges on the context's connection (its lock held). A failure on the
 * way (a timeout, a closed connection, an answer that makes no sense)
 * leaves the connection at an unknown place in the protocol: a late answer
 * may still come and would be read as the next one's. So the connection is
 * dropped, and the next exchange opens a new one. _send begins an exchange
 * (opening the connection if needed), the others continue it. An answer's
 * number comes back in *value: a negative one is the server's error, the
 * connection is still in step. */
int iio_net_ctx_send(struct iio_context *ctx, const char *cmd);
int iio_net_ctx_command(struct iio_context *ctx, const char *cmd, int *value);
int iio_net_ctx_write(struct iio_context *ctx, const void *src, size_t len);
int iio_net_ctx_integer(struct iio_context *ctx, int *value);
int iio_net_ctx_read(struct iio_context *ctx, void *dst, size_t len);
void iio_net_ctx_drop(struct iio_context *ctx);

ssize_t iio_net_attr_read(const struct iio_device *dev, const struct iio_channel *chn,
                          enum iio_attr_kind kind, const char *attr, char *dst, size_t len);
ssize_t iio_net_attr_write(const struct iio_device *dev, const struct iio_channel *chn,
                           enum iio_attr_kind kind, const char *attr, const void *src,
                           size_t len);

/* Buffers over their own connection (iionet.c). */
int iio_net_open(struct iio_buffer *buf);
void iio_net_close(struct iio_buffer *buf);
ssize_t iio_net_readbuf(struct iio_buffer *buf, void *dst, size_t len);
ssize_t iio_net_writebuf(struct iio_buffer *buf, const void *src, size_t len);

/* A copy of a string, or 0 (malloc failed). */
char *iio_strdup(const char *s);

#endif
