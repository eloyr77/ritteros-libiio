/*
 * libiio: contexts, devices, channels and their attributes, the
 * samples' conversion and buffers, as libiio 0.26's interface has them
 * (iio.h). A context comes from an XML description (iioxml.c); a network
 * context fetches it from iiod and reads and writes through it (iionet.c).
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "ritteros.h"
#include "iiopriv.h"

#define IIOD_PORT        30431
#define DEFAULT_TIMEOUT  5000          /* ms, as libiio's network backend */
#define ATTR_MAX         0x10000       /* the longest value read_all reads */

static void *fail(int err)
{
    errno = err < 0 ? -err : err;
    return 0;
}

/* --- Channel types, from the ID --- */

static const char *const type_names[] = {
    [IIO_VOLTAGE] = "voltage", [IIO_CURRENT] = "current", [IIO_POWER] = "power",
    [IIO_ACCEL] = "accel", [IIO_ANGL_VEL] = "anglvel", [IIO_MAGN] = "magn",
    [IIO_LIGHT] = "illuminance", [IIO_INTENSITY] = "intensity", [IIO_PROXIMITY] = "proximity",
    [IIO_TEMP] = "temp", [IIO_INCLI] = "incli", [IIO_ROT] = "rot", [IIO_ANGL] = "angl",
    [IIO_TIMESTAMP] = "timestamp", [IIO_CAPACITANCE] = "capacitance",
    [IIO_ALTVOLTAGE] = "altvoltage", [IIO_CCT] = "cct", [IIO_PRESSURE] = "pressure",
    [IIO_HUMIDITYRELATIVE] = "humidityrelative", [IIO_ACTIVITY] = "activity",
    [IIO_STEPS] = "steps", [IIO_ENERGY] = "energy", [IIO_DISTANCE] = "distance",
    [IIO_VELOCITY] = "velocity", [IIO_CONCENTRATION] = "concentration",
    [IIO_RESISTANCE] = "resistance", [IIO_PH] = "ph", [IIO_UVINDEX] = "uvindex",
    [IIO_ELECTRICALCONDUCTIVITY] = "electricalconductivity", [IIO_COUNT] = "count",
    [IIO_INDEX] = "index", [IIO_GRAVITY] = "gravity",
    [IIO_POSITIONRELATIVE] = "positionrelative", [IIO_PHASE] = "phase",
    [IIO_MASSCONCENTRATION] = "massconcentration",
};

static const char *const hwmon_names[] = {
    [HWMON_VOLTAGE] = "in", [HWMON_FAN] = "fan", [HWMON_PWM] = "pwm", [HWMON_TEMP] = "temp",
    [HWMON_CURRENT] = "curr", [HWMON_POWER] = "power", [HWMON_ENERGY] = "energy",
    [HWMON_HUMIDITY] = "humidity", [HWMON_INTRUSION] = "intrusion",
};

static const char *const modifier_names[] = {
    [IIO_MOD_X] = "x", [IIO_MOD_Y] = "y", [IIO_MOD_Z] = "z", [IIO_MOD_X_AND_Y] = "x&y",
    [IIO_MOD_X_AND_Z] = "x&z", [IIO_MOD_Y_AND_Z] = "y&z", [IIO_MOD_X_AND_Y_AND_Z] = "x&y&z",
    [IIO_MOD_X_OR_Y] = "x|y", [IIO_MOD_X_OR_Z] = "x|z", [IIO_MOD_Y_OR_Z] = "y|z",
    [IIO_MOD_X_OR_Y_OR_Z] = "x|y|z", [IIO_MOD_ROOT_SUM_SQUARED_X_Y] = "sqrt(x^2+y^2)",
    [IIO_MOD_SUM_SQUARED_X_Y_Z] = "x^2+y^2+z^2", [IIO_MOD_LIGHT_BOTH] = "both",
    [IIO_MOD_LIGHT_IR] = "ir", [IIO_MOD_LIGHT_CLEAR] = "clear", [IIO_MOD_LIGHT_RED] = "red",
    [IIO_MOD_LIGHT_GREEN] = "green", [IIO_MOD_LIGHT_BLUE] = "blue", [IIO_MOD_LIGHT_UV] = "uv",
    [IIO_MOD_LIGHT_DUV] = "duv", [IIO_MOD_QUATERNION] = "quaternion",
    [IIO_MOD_TEMP_AMBIENT] = "ambient", [IIO_MOD_TEMP_OBJECT] = "object",
    [IIO_MOD_NORTH_MAGN] = "from_north_magnetic", [IIO_MOD_NORTH_TRUE] = "from_north_true",
    [IIO_MOD_NORTH_MAGN_TILT_COMP] = "from_north_magnetic_tilt_comp",
    [IIO_MOD_NORTH_TRUE_TILT_COMP] = "from_north_true_tilt_comp",
    [IIO_MOD_RUNNING] = "running", [IIO_MOD_JOGGING] = "jogging", [IIO_MOD_WALKING] = "walking",
    [IIO_MOD_STILL] = "still", [IIO_MOD_ROOT_SUM_SQUARED_X_Y_Z] = "sqrt(x^2+y^2+z^2)",
    [IIO_MOD_I] = "i", [IIO_MOD_Q] = "q", [IIO_MOD_CO2] = "co2", [IIO_MOD_ETHANOL] = "ethanol",
    [IIO_MOD_H2] = "h2", [IIO_MOD_O2] = "o2", [IIO_MOD_VOC] = "voc", [IIO_MOD_PM1] = "pm1",
    [IIO_MOD_PM2P5] = "pm2p5", [IIO_MOD_PM4] = "pm4", [IIO_MOD_PM10] = "pm10",
    [IIO_MOD_LINEAR_X] = "x-g", [IIO_MOD_LINEAR_Y] = "y-g", [IIO_MOD_LINEAR_Z] = "z-g",
    [IIO_MOD_PITCH] = "pitch", [IIO_MOD_YAW] = "yaw", [IIO_MOD_ROLL] = "roll",
};

/* The type whose name begins the ID and is followed by its end, a '_' or
 * a digit ("voltage0", "altvoltage1", "in_voltage"...); -1 for none. */
static int find_type(const char *id, const char *const *names, int count)
{
    for (int i = 0; i < count; i++) {
        size_t n = strlen(names[i]);
        if (strncmp(names[i], id, n))
            continue;
        char c = id[n];
        if (c == 0 || c == '_' || (c >= '0' && c <= '9'))
            return i;
    }
    return -1;
}

void iio_channel_finish(struct iio_channel *chn)
{
    int t = iio_device_is_hwmon(chn->dev)
          ? find_type(chn->id, hwmon_names, (int)(sizeof(hwmon_names) / sizeof(hwmon_names[0])))
          : find_type(chn->id, type_names, (int)(sizeof(type_names) / sizeof(type_names[0])));
    chn->type = t >= 0 ? (enum iio_chan_type)t : IIO_CHAN_TYPE_UNKNOWN;
    chn->modifier = IIO_NO_MOD;
    const char *mod = strchr(chn->id, '_');
    if (!mod)
        return;
    mod++;
    for (unsigned i = 0; i < sizeof(modifier_names) / sizeof(modifier_names[0]); i++)
        if (modifier_names[i] && !strncmp(modifier_names[i], mod, strlen(modifier_names[i]))) {
            chn->modifier = (enum iio_modifier)i;
            break;
        }
}

/* --- The library --- */

void iio_library_get_version(unsigned int *major, unsigned int *minor, char git_tag[8])
{
    if (major)
        *major = 0;
    if (minor)
        *minor = 26;
    if (git_tag)
        strcpy(git_tag, "ritter");
}

void iio_strerror(int err, char *dst, size_t len)
{
    if (len)
        snprintf(dst, len, "%s", strerror(err < 0 ? -err : err));
}

static const char *const backends[] = { "ip", "xml" };

bool iio_has_backend(const char *backend)
{
    for (unsigned i = 0; i < sizeof(backends) / sizeof(backends[0]); i++)
        if (!strcmp(backend, backends[i]))
            return true;
    return false;
}

unsigned int iio_get_backends_count(void)
{
    return sizeof(backends) / sizeof(backends[0]);
}

const char *iio_get_backend(unsigned int index)
{
    return index < iio_get_backends_count() ? backends[index] : 0;
}

/* --- Scanning: no discovery yet, so nothing is found --- */

struct iio_scan_context {
    int unused;
};
struct iio_scan_block {
    int unused;
};

struct iio_scan_context *iio_create_scan_context(const char *backend, unsigned int flags)
{
    (void)backend, (void)flags;
    return calloc(1, sizeof(struct iio_scan_context));
}

void iio_scan_context_destroy(struct iio_scan_context *ctx)
{
    free(ctx);
}

ssize_t iio_scan_context_get_info_list(struct iio_scan_context *ctx,
                                       struct iio_context_info ***info)
{
    (void)ctx;
    *info = calloc(1, sizeof(struct iio_context_info *));
    return *info ? 0 : -ENOMEM;
}

void iio_context_info_list_free(struct iio_context_info **info)
{
    free(info);
}

const char *iio_context_info_get_description(const struct iio_context_info *info)
{
    (void)info;
    return 0;
}

const char *iio_context_info_get_uri(const struct iio_context_info *info)
{
    (void)info;
    return 0;
}

struct iio_scan_block *iio_create_scan_block(const char *backend, unsigned int flags)
{
    (void)backend, (void)flags;
    return calloc(1, sizeof(struct iio_scan_block));
}

void iio_scan_block_destroy(struct iio_scan_block *blk)
{
    free(blk);
}

ssize_t iio_scan_block_scan(struct iio_scan_block *blk)
{
    (void)blk;
    return 0;
}

struct iio_context_info *iio_scan_block_get_info(struct iio_scan_block *blk, unsigned int index)
{
    (void)blk, (void)index;
    return fail(EINVAL);
}

/* --- Contexts --- */

struct iio_context *iio_context_new(const char *name)
{
    struct iio_context *ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return 0;
    ctx->name = iio_strdup(name);
    ctx->fd = -1;
    ctx->timeout_ms = DEFAULT_TIMEOUT;
    iio_library_get_version(&ctx->major, &ctx->minor, ctx->git_tag);
    if (!ctx->name || pthread_mutex_init(&ctx->lock.mutex, 0) != 0) {
        free(ctx->name);
        free(ctx);
        return 0;
    }
    return ctx;
}

static void free_attrs(struct iio_attr *a, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        free(a[i].name), free(a[i].filename);
    free(a);
}

void iio_context_destroy(struct iio_context *ctx)
{
    if (!ctx)
        return;
    for (unsigned i = 0; i < ctx->ndevices; i++) {
        struct iio_device *dev = ctx->devices[i];
        if (dev->buffer)
            iio_buffer_destroy(dev->buffer);
        for (unsigned k = 0; k < dev->nchannels; k++) {
            struct iio_channel *c = dev->channels[k];
            free(c->id), free(c->name);
            free_attrs(c->attrs, c->nattrs);
            free(c);
        }
        free(dev->channels);
        free_attrs(dev->attrs, dev->nattrs);
        free_attrs(dev->buffer_attrs, dev->nbuffer_attrs);
        free_attrs(dev->debug_attrs, dev->ndebug_attrs);
        free(dev->id), free(dev->name), free(dev->label), free(dev->mask);
        free(dev);
    }
    free(ctx->devices);
    for (unsigned i = 0; i < ctx->nattrs; i++)
        free(ctx->attr_names[i]), free(ctx->attr_values[i]);
    free(ctx->attr_names), free(ctx->attr_values);
    if (ctx->fd >= 0) {
        iio_net_write_all(ctx->fd, "\r\nEXIT\r\n", 8);
        close(ctx->fd);
    }
    pthread_mutex_destroy(&ctx->lock.mutex);
    free(ctx->name), free(ctx->description), free(ctx->xml), free(ctx->host);
    free(ctx);
}

static struct iio_context *from_xml(const char *name, const char *xml, size_t len)
{
    struct iio_context *ctx = iio_context_new(name);
    if (!ctx)
        return fail(ENOMEM);
    ctx->xml = malloc(len + 1);
    if (!ctx->xml) {
        iio_context_destroy(ctx);
        return fail(ENOMEM);
    }
    memcpy(ctx->xml, xml, len);
    ctx->xml[len] = 0;
    int ret = iio_xml_parse(ctx, ctx->xml, len);
    if (ret < 0) {
        iio_context_destroy(ctx);
        return fail(ret);
    }
    return ctx;
}

struct iio_context *iio_create_xml_context_mem(const char *xml, size_t len)
{
    return from_xml("xml", xml, len);
}

struct iio_context *iio_create_xml_context(const char *xml_file)
{
    int fd = open(xml_file, O_RDONLY);
    if (fd < 0)
        return 0;
    size_t size = 0, cap = 0;
    char *text = 0;
    for (;;) {
        if (size + 4096 > cap) {
            char *bigger = realloc(text, cap = cap * 2 + 8192);
            if (!bigger) {
                free(text);
                close(fd);
                return fail(ENOMEM);
            }
            text = bigger;
        }
        ssize_t n = read(fd, text + size, cap - size);
        if (n < 0) {
            int err = errno;
            free(text);
            close(fd);
            return fail(err);
        }
        if (n == 0)
            break;
        size += (size_t)n;
    }
    close(fd);
    struct iio_context *ctx = from_xml("xml", text, size);
    free(text);
    return ctx;
}

struct iio_context *iio_create_default_context(void)
{
    return fail(ENOSYS);
}

struct iio_context *iio_create_local_context(void)
{
    return fail(ENOSYS);
}

/* "HOST" or "HOST:PORT" into an address and a port. */
static int host_address(const char *host, uint32_t *ip, uint16_t *port)
{
    char name[256];
    const char *colon = strrchr(host, ':');
    size_t n = colon ? (size_t)(colon - host) : strlen(host);
    *port = IIOD_PORT;
    if (n == 0 || n >= sizeof(name))
        return -EINVAL;
    memcpy(name, host, n);
    name[n] = 0;
    if (colon) {
        char *end;
        unsigned long p = strtoul(colon + 1, &end, 10);
        if (end == colon + 1 || *end || p == 0 || p > 65535)
            return -EINVAL;
        *port = (uint16_t)p;
    }
    if (ritteros_resolve(name, ip) < 0)
        return -errno;
    return 0;
}

/* iiod's VERSION: "0.26.ab12cd3" (major, minor, the git tag). */
static int fetch_version(struct iio_context *ctx)
{
    int ret = iio_net_write_all(ctx->fd, "VERSION\r\n", 9);
    if (ret < 0)
        return ret;
    char line[64];
    size_t n = 0;
    for (;;) {
        char c;
        ret = iio_net_read_all(ctx->fd, ctx->timeout_ms, &c, 1);
        if (ret < 0)
            return ret;
        if (c == '\n')
            break;
        if (n + 1 < sizeof(line) && c != '\r')
            line[n++] = c;
    }
    line[n] = 0;
    char *end, *p = line;
    unsigned long major = strtoul(p, &end, 10);
    if (end == p || *end != '.')
        return -EIO;
    p = end + 1;
    unsigned long minor = strtoul(p, &end, 10);
    if (end == p || *end != '.')
        return -EIO;
    ctx->major = (unsigned)major;
    ctx->minor = (unsigned)minor;
    memset(ctx->git_tag, 0, sizeof(ctx->git_tag));
    strncpy(ctx->git_tag, end + 1, sizeof(ctx->git_tag) - 1);
    return 0;
}

struct iio_context *iio_create_network_context(const char *host)
{
    uint32_t ip;
    uint16_t port;
    if (!host || !host[0])
        return fail(EINVAL);                /* (no discovery: give the host) */
    int ret = host_address(host, &ip, &port);
    if (ret < 0)
        return fail(ret);
    int fd = iio_net_connect(ip, port, DEFAULT_TIMEOUT);
    if (fd < 0)
        return fail(fd);
    /* The description first, into a context of its own. */
    ret = iio_net_command(fd, DEFAULT_TIMEOUT, "PRINT\r\n");
    char *xml = 0;
    if (ret >= 0) {
        size_t len = (size_t)ret;
        xml = malloc(len + 1);
        ret = xml ? iio_net_read_all(fd, DEFAULT_TIMEOUT, xml, len + 1) : -ENOMEM;  /* (+ "\n") */
        if (ret >= 0)
            ret = (int)len;
    }
    if (ret < 0) {
        free(xml);
        close(fd);
        return fail(ret);
    }
    struct iio_context *ctx = from_xml("network", xml, (size_t)ret);
    free(xml);
    if (!ctx) {
        int err = errno;
        close(fd);
        return fail(err);
    }
    ctx->network = true;
    ctx->fd = fd;
    ctx->ip = ip;
    ctx->port = port;
    ctx->host = iio_strdup(host);
    char text[300];
    snprintf(text, sizeof(text), "%u.%u.%u.%u", ip >> 24, ip >> 16 & 255, ip >> 8 & 255, ip & 255);
    if (!ctx->description)
        ctx->description = iio_strdup(text);
    ret = fetch_version(ctx);
    if (ret == 0)
        ret = iio_context_add_attr(ctx, "ip,ip-addr", text);
    if (ret == 0) {
        snprintf(text, sizeof(text), "ip:%s", host);
        ret = iio_context_add_attr(ctx, "uri", text);
    }
    if (ret < 0 || !ctx->host) {
        iio_context_destroy(ctx);
        return fail(ret < 0 ? ret : -ENOMEM);
    }
    return ctx;
}

struct iio_context *iio_create_context_from_uri(const char *uri)
{
    if (!strncmp(uri, "ip:", 3))
        return iio_create_network_context(uri + 3);
    if (!strncmp(uri, "xml:", 4))
        return iio_create_xml_context(uri + 4);
    return fail(ENOSYS);
}

struct iio_context *iio_context_clone(const struct iio_context *ctx)
{
    if (ctx->network)
        return iio_create_network_context(ctx->host);
    return iio_create_xml_context_mem(ctx->xml, strlen(ctx->xml));
}

int iio_context_get_version(const struct iio_context *ctx, unsigned int *major,
                            unsigned int *minor, char git_tag[8])
{
    if (major)
        *major = ctx->major;
    if (minor)
        *minor = ctx->minor;
    if (git_tag)
        memcpy(git_tag, ctx->git_tag, 8);
    return 0;
}

const char *iio_context_get_xml(const struct iio_context *ctx)
{
    return ctx->xml;
}

const char *iio_context_get_name(const struct iio_context *ctx)
{
    return ctx->name;
}

const char *iio_context_get_description(const struct iio_context *ctx)
{
    return ctx->description ? ctx->description : "";
}

unsigned int iio_context_get_attrs_count(const struct iio_context *ctx)
{
    return ctx->nattrs;
}

int iio_context_get_attr(const struct iio_context *ctx, unsigned int index, const char **name,
                         const char **value)
{
    if (index >= ctx->nattrs)
        return -EINVAL;
    if (name)
        *name = ctx->attr_names[index];
    if (value)
        *value = ctx->attr_values[index];
    return 0;
}

const char *iio_context_get_attr_value(const struct iio_context *ctx, const char *name)
{
    for (unsigned i = 0; i < ctx->nattrs; i++)
        if (!strcmp(ctx->attr_names[i], name))
            return ctx->attr_values[i];
    return 0;
}

unsigned int iio_context_get_devices_count(const struct iio_context *ctx)
{
    return ctx->ndevices;
}

struct iio_device *iio_context_get_device(const struct iio_context *ctx, unsigned int index)
{
    return index < ctx->ndevices ? ctx->devices[index] : 0;
}

struct iio_device *iio_context_find_device(const struct iio_context *ctx, const char *name)
{
    for (unsigned i = 0; i < ctx->ndevices; i++) {
        struct iio_device *d = ctx->devices[i];
        if (!strcmp(d->id, name) || (d->label && !strcmp(d->label, name)) ||
            (d->name && !strcmp(d->name, name)))
            return d;
    }
    return 0;
}

int iio_context_set_timeout(struct iio_context *ctx, unsigned int timeout_ms)
{
    if (ctx->network) {
        char cmd[48];
        snprintf(cmd, sizeof(cmd), "TIMEOUT %u\r\n", timeout_ms);
        iio_lock_take(&ctx->lock);
        int value;
        int ret = iio_net_ctx_command(ctx, cmd, &value);
        if (ret == 0 && value < 0)
            ret = value;
        if (ret == 0) {
            ctx->timeout_ms = timeout_ms;
            ctx->timeout_sent = true;           /* (for a new connection: iionet.c) */
        }
        iio_lock_give(&ctx->lock);
        return ret;
    }
    ctx->timeout_ms = timeout_ms;
    return 0;
}

/* --- Devices --- */

const struct iio_context *iio_device_get_context(const struct iio_device *dev)
{
    return dev->ctx;
}

const char *iio_device_get_id(const struct iio_device *dev)
{
    return dev->id;
}

const char *iio_device_get_name(const struct iio_device *dev)
{
    return dev->name;
}

const char *iio_device_get_label(const struct iio_device *dev)
{
    return dev->label;
}

unsigned int iio_device_get_channels_count(const struct iio_device *dev)
{
    return dev->nchannels;
}

struct iio_channel *iio_device_get_channel(const struct iio_device *dev, unsigned int index)
{
    return index < dev->nchannels ? dev->channels[index] : 0;
}

struct iio_channel *iio_device_find_channel(const struct iio_device *dev, const char *name,
                                            bool output)
{
    for (unsigned i = 0; i < dev->nchannels; i++) {
        struct iio_channel *c = dev->channels[i];
        if (c->output == output && (!strcmp(c->id, name) || (c->name && !strcmp(c->name, name))))
            return c;
    }
    return 0;
}

static const char *list_get(const struct iio_attr *a, unsigned n, unsigned index)
{
    return index < n ? a[index].name : 0;
}

static const char *list_find(const struct iio_attr *a, unsigned n, const char *name)
{
    for (unsigned i = 0; i < n; i++)
        if (!strcmp(a[i].name, name))
            return a[i].name;
    return 0;
}

unsigned int iio_device_get_attrs_count(const struct iio_device *dev)
{
    return dev->nattrs;
}

const char *iio_device_get_attr(const struct iio_device *dev, unsigned int index)
{
    return list_get(dev->attrs, dev->nattrs, index);
}

const char *iio_device_find_attr(const struct iio_device *dev, const char *name)
{
    return list_find(dev->attrs, dev->nattrs, name);
}

unsigned int iio_device_get_buffer_attrs_count(const struct iio_device *dev)
{
    return dev->nbuffer_attrs;
}

const char *iio_device_get_buffer_attr(const struct iio_device *dev, unsigned int index)
{
    return list_get(dev->buffer_attrs, dev->nbuffer_attrs, index);
}

const char *iio_device_find_buffer_attr(const struct iio_device *dev, const char *name)
{
    return list_find(dev->buffer_attrs, dev->nbuffer_attrs, name);
}

unsigned int iio_device_get_debug_attrs_count(const struct iio_device *dev)
{
    return dev->ndebug_attrs;
}

const char *iio_device_get_debug_attr(const struct iio_device *dev, unsigned int index)
{
    return list_get(dev->debug_attrs, dev->ndebug_attrs, index);
}

const char *iio_device_find_debug_attr(const struct iio_device *dev, const char *name)
{
    return list_find(dev->debug_attrs, dev->ndebug_attrs, name);
}

int iio_device_identify_filename(const struct iio_device *dev, const char *filename,
                                 struct iio_channel **chn, const char **attr)
{
    for (unsigned i = 0; i < dev->nchannels; i++) {
        struct iio_channel *c = dev->channels[i];
        for (unsigned k = 0; k < c->nattrs; k++)
            if (c->attrs[k].filename && !strcmp(c->attrs[k].filename, filename)) {
                *attr = c->attrs[k].name;
                *chn = c;
                return 0;
            }
    }
    const char *a = list_find(dev->attrs, dev->nattrs, filename);
    if (!a)
        a = list_find(dev->debug_attrs, dev->ndebug_attrs, filename);
    if (!a)
        return -EINVAL;
    *attr = a;
    *chn = 0;
    return 0;
}

void iio_device_set_data(struct iio_device *dev, void *data)
{
    dev->userdata = data;
}

void *iio_device_get_data(const struct iio_device *dev)
{
    return dev->userdata;
}

bool iio_device_is_trigger(const struct iio_device *dev)
{
    return dev->nchannels == 0 && dev->name && !strncmp(dev->id, "trigger", 7);
}

bool iio_device_is_hwmon(const struct iio_device *dev)
{
    return dev->id[0] == 'h';
}

int iio_device_get_trigger(const struct iio_device *dev, const struct iio_device **trigger)
{
    struct iio_context *ctx = dev->ctx;
    if (!ctx->network)
        return -ENOSYS;
    char cmd[320], name[256];
    snprintf(cmd, sizeof(cmd), "GETTRIG %s\r\n", dev->id);
    iio_lock_take(&ctx->lock);
    int length;
    int ret = iio_net_ctx_command(ctx, cmd, &length);
    if (ret == 0 && length < 0) {
        ret = length;
    } else if (ret == 0 && (size_t)length + 1 > sizeof(name)) {
        iio_net_ctx_drop(ctx);              /* (out of step: no names are this long) */
        ret = -EIO;
    } else if (ret == 0 && length > 0) {
        ret = iio_net_ctx_read(ctx, name, (size_t)length + 1);
    }
    iio_lock_give(&ctx->lock);
    if (ret < 0)
        return ret;
    *trigger = 0;
    if (length == 0)
        return 0;
    name[length] = 0;
    for (unsigned i = 0; i < ctx->ndevices; i++) {
        const struct iio_device *d = ctx->devices[i];
        if (iio_device_is_trigger(d) && !strcmp(d->name, name)) {
            *trigger = d;
            return 0;
        }
    }
    return -ENXIO;
}

int iio_device_set_trigger(const struct iio_device *dev, const struct iio_device *trigger)
{
    struct iio_context *ctx = dev->ctx;
    if (!ctx->network)
        return -ENOSYS;
    char cmd[320];
    if (trigger)
        snprintf(cmd, sizeof(cmd), "SETTRIG %s %s\r\n", dev->id, trigger->id);
    else
        snprintf(cmd, sizeof(cmd), "SETTRIG %s\r\n", dev->id);
    iio_lock_take(&ctx->lock);
    int value;
    int ret = iio_net_ctx_command(ctx, cmd, &value);
    iio_lock_give(&ctx->lock);
    return ret < 0 ? ret : value < 0 ? value : 0;
}

int iio_device_set_kernel_buffers_count(const struct iio_device *dev, unsigned int nb_buffers)
{
    struct iio_context *ctx = dev->ctx;
    if (!ctx->network)
        return -ENOSYS;
    if (nb_buffers == 0)
        return -EINVAL;
    char cmd[320];
    snprintf(cmd, sizeof(cmd), "SET %s BUFFERS_COUNT %u\r\n", dev->id, nb_buffers);
    iio_lock_take(&ctx->lock);
    int value;
    int ret = iio_net_ctx_command(ctx, cmd, &value);
    iio_lock_give(&ctx->lock);
    return ret < 0 ? ret : value < 0 ? value : 0;
}

/* --- Attributes: one set of helpers for the four kinds --- */

static bool known(const struct iio_device *dev, const struct iio_channel *chn,
                  enum iio_attr_kind kind, const char *attr)
{
    switch (kind) {
    case ATTR_CHANNEL:
        return list_find(chn->attrs, chn->nattrs, attr);
    case ATTR_BUFFER:
        return list_find(dev->buffer_attrs, dev->nbuffer_attrs, attr);
    case ATTR_DEBUG:
        return list_find(dev->debug_attrs, dev->ndebug_attrs, attr);
    default:
        return list_find(dev->attrs, dev->nattrs, attr);
    }
}

static ssize_t attr_read(const struct iio_device *dev, const struct iio_channel *chn,
                         enum iio_attr_kind kind, const char *attr, char *dst, size_t len)
{
    if (!attr || !known(dev, chn, kind, attr))
        return -ENOENT;
    return iio_net_attr_read(dev, chn, kind, attr, dst, len);
}

static ssize_t attr_write_raw(const struct iio_device *dev, const struct iio_channel *chn,
                              enum iio_attr_kind kind, const char *attr, const void *src,
                              size_t len)
{
    if (!attr || !known(dev, chn, kind, attr))
        return -ENOENT;
    return iio_net_attr_write(dev, chn, kind, attr, src, len);
}

static ssize_t attr_write(const struct iio_device *dev, const struct iio_channel *chn,
                          enum iio_attr_kind kind, const char *attr, const char *src)
{
    return attr_write_raw(dev, chn, kind, attr, src, strlen(src) + 1);   /* (with its 0, as libiio) */
}

static int attr_read_longlong(const struct iio_device *dev, const struct iio_channel *chn,
                              enum iio_attr_kind kind, const char *attr, long long *val)
{
    char buf[1024], *end;
    ssize_t ret = attr_read(dev, chn, kind, attr, buf, sizeof(buf));
    if (ret < 0)
        return (int)ret;
    errno = 0;
    long long v = strtoll(buf, &end, 0);
    if (end == buf || errno == ERANGE)
        return -EINVAL;
    *val = v;
    return 0;
}

static int attr_read_bool(const struct iio_device *dev, const struct iio_channel *chn,
                          enum iio_attr_kind kind, const char *attr, bool *val)
{
    long long v;
    int ret = attr_read_longlong(dev, chn, kind, attr, &v);
    if (ret == 0)
        *val = v != 0;
    return ret;
}

static int attr_read_double(const struct iio_device *dev, const struct iio_channel *chn,
                            enum iio_attr_kind kind, const char *attr, double *val)
{
    char buf[1024], *end;
    ssize_t ret = attr_read(dev, chn, kind, attr, buf, sizeof(buf));
    if (ret < 0)
        return (int)ret;
    double v = strtod(buf, &end);
    if (end == buf)
        return -EINVAL;
    *val = v;
    return 0;
}

static int written(ssize_t ret)
{
    return ret < 0 ? (int)ret : 0;
}

static int attr_write_longlong(const struct iio_device *dev, const struct iio_channel *chn,
                               enum iio_attr_kind kind, const char *attr, long long val)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", val);
    return written(attr_write(dev, chn, kind, attr, buf));
}

static int attr_write_bool(const struct iio_device *dev, const struct iio_channel *chn,
                           enum iio_attr_kind kind, const char *attr, bool val)
{
    return written(attr_write(dev, chn, kind, attr, val ? "1" : "0"));
}

static int attr_write_double(const struct iio_device *dev, const struct iio_channel *chn,
                             enum iio_attr_kind kind, const char *attr, double val)
{
    char buf[64];
    if (val != val || val > 1e12 || val < -1e12)
        return -EINVAL;                     /* (printf's %f here keeps 19 digits) */
    snprintf(buf, sizeof(buf), "%f", val);
    return written(attr_write(dev, chn, kind, attr, buf));
}

/* The names of the attributes of a kind. */
static void attr_list(const struct iio_device *dev, const struct iio_channel *chn,
                      enum iio_attr_kind kind, const struct iio_attr **list, unsigned *n)
{
    switch (kind) {
    case ATTR_CHANNEL:
        *list = chn->attrs, *n = chn->nattrs;
        break;
    case ATTR_BUFFER:
        *list = dev->buffer_attrs, *n = dev->nbuffer_attrs;
        break;
    case ATTR_DEBUG:
        *list = dev->debug_attrs, *n = dev->ndebug_attrs;
        break;
    default:
        *list = dev->attrs, *n = dev->nattrs;
    }
}

/* read_all: every attribute that can be read, one by one (a failed one is
 * left out, as libiio leaves out a negative length). */
static int read_all(struct iio_device *dev, struct iio_channel *chn, enum iio_attr_kind kind,
                    int (*dcb)(struct iio_device *, const char *, const char *, size_t, void *),
                    int (*ccb)(struct iio_channel *, const char *, const char *, size_t, void *),
                    void *data)
{
    const struct iio_attr *list;
    unsigned n;
    attr_list(dev, chn, kind, &list, &n);
    char *buf = malloc(ATTR_MAX);
    if (!buf)
        return -ENOMEM;
    int ret = 0;
    for (unsigned i = 0; i < n && ret >= 0; i++) {
        ssize_t len = attr_read(dev, chn, kind, list[i].name, buf, ATTR_MAX);
        if (len < 0) {
            if (len == -ENOMEM || len == -EPIPE || len == -ETIMEDOUT) {
                ret = (int)len;             /* (the connection is gone, not the value) */
                break;
            }
            continue;
        }
        ret = chn ? ccb(chn, list[i].name, buf, (size_t)len, data)
                  : dcb(dev, list[i].name, buf, (size_t)len, data);
    }
    free(buf);
    return ret < 0 ? ret : 0;
}

static int write_all(struct iio_device *dev, struct iio_channel *chn, enum iio_attr_kind kind,
                     ssize_t (*dcb)(struct iio_device *, const char *, void *, size_t, void *),
                     ssize_t (*ccb)(struct iio_channel *, const char *, void *, size_t, void *),
                     void *data)
{
    const struct iio_attr *list;
    unsigned n;
    attr_list(dev, chn, kind, &list, &n);
    char *buf = malloc(ATTR_MAX);
    if (!buf)
        return -ENOMEM;
    int ret = 0;
    for (unsigned i = 0; i < n; i++) {
        ssize_t len = chn ? ccb(chn, list[i].name, buf, ATTR_MAX, data)
                          : dcb(dev, list[i].name, buf, ATTR_MAX, data);
        if (len < 0) {
            ret = (int)len;
            break;
        }
        if (len == 0)
            continue;
        ssize_t w = attr_write_raw(dev, chn, kind, list[i].name, buf, (size_t)len);
        if (w < 0) {
            ret = (int)w;
            break;
        }
    }
    free(buf);
    return ret;
}

/* The public calls: a set of eleven per kind of device attribute. */
#define DEVICE_ATTR_CALLS(prefix, kind)                                                        \
    ssize_t prefix##_read(const struct iio_device *dev, const char *attr, char *dst,           \
                          size_t len)                                                          \
    {                                                                                          \
        return attr_read(dev, 0, kind, attr, dst, len);                                        \
    }                                                                                          \
    int prefix##_read_all(struct iio_device *dev,                                              \
                          int (*cb)(struct iio_device *, const char *, const char *, size_t,   \
                                    void *),                                                   \
                          void *data)                                                          \
    {                                                                                          \
        return read_all(dev, 0, kind, cb, 0, data);                                            \
    }                                                                                          \
    int prefix##_read_bool(const struct iio_device *dev, const char *attr, bool *val)          \
    {                                                                                          \
        return attr_read_bool(dev, 0, kind, attr, val);                                        \
    }                                                                                          \
    int prefix##_read_longlong(const struct iio_device *dev, const char *attr, long long *val)  \
    {                                                                                          \
        return attr_read_longlong(dev, 0, kind, attr, val);                                    \
    }                                                                                          \
    int prefix##_read_double(const struct iio_device *dev, const char *attr, double *val)      \
    {                                                                                          \
        return attr_read_double(dev, 0, kind, attr, val);                                      \
    }                                                                                          \
    ssize_t prefix##_write(const struct iio_device *dev, const char *attr, const char *src)    \
    {                                                                                          \
        return attr_write(dev, 0, kind, attr, src);                                            \
    }                                                                                          \
    ssize_t prefix##_write_raw(const struct iio_device *dev, const char *attr,                 \
                               const void *src, size_t len)                                    \
    {                                                                                          \
        return attr_write_raw(dev, 0, kind, attr, src, len);                                   \
    }                                                                                          \
    int prefix##_write_all(struct iio_device *dev,                                             \
                           ssize_t (*cb)(struct iio_device *, const char *, void *, size_t,    \
                                         void *),                                              \
                           void *data)                                                         \
    {                                                                                          \
        return write_all(dev, 0, kind, cb, 0, data);                                           \
    }                                                                                          \
    int prefix##_write_bool(const struct iio_device *dev, const char *attr, bool val)          \
    {                                                                                          \
        return attr_write_bool(dev, 0, kind, attr, val);                                       \
    }                                                                                          \
    int prefix##_write_longlong(const struct iio_device *dev, const char *attr, long long val)  \
    {                                                                                          \
        return attr_write_longlong(dev, 0, kind, attr, val);                                   \
    }                                                                                          \
    int prefix##_write_double(const struct iio_device *dev, const char *attr, double val)      \
    {                                                                                          \
        return attr_write_double(dev, 0, kind, attr, val);                                     \
    }

DEVICE_ATTR_CALLS(iio_device_attr, ATTR_DEVICE)
DEVICE_ATTR_CALLS(iio_device_buffer_attr, ATTR_BUFFER)
DEVICE_ATTR_CALLS(iio_device_debug_attr, ATTR_DEBUG)

int iio_device_reg_write(struct iio_device *dev, uint32_t address, uint32_t value)
{
    char text[32];
    snprintf(text, sizeof(text), "0x%x 0x%x", (unsigned)address, (unsigned)value);
    return written(iio_device_debug_attr_write(dev, "direct_reg_access", text));
}

int iio_device_reg_read(struct iio_device *dev, uint32_t address, uint32_t *value)
{
    char text[32];
    snprintf(text, sizeof(text), "0x%x", (unsigned)address);
    ssize_t ret = iio_device_debug_attr_write(dev, "direct_reg_access", text);
    if (ret < 0)
        return (int)ret;
    long long v;
    ret = iio_device_debug_attr_read_longlong(dev, "direct_reg_access", &v);
    if (ret == 0)
        *value = (uint32_t)v;
    return (int)ret;
}

/* --- Channels --- */

const struct iio_device *iio_channel_get_device(const struct iio_channel *chn)
{
    return chn->dev;
}

const char *iio_channel_get_id(const struct iio_channel *chn)
{
    return chn->id;
}

const char *iio_channel_get_name(const struct iio_channel *chn)
{
    return chn->name;
}

bool iio_channel_is_output(const struct iio_channel *chn)
{
    return chn->output;
}

bool iio_channel_is_scan_element(const struct iio_channel *chn)
{
    return chn->scan_element;
}

enum iio_chan_type iio_channel_get_type(const struct iio_channel *chn)
{
    return chn->type;
}

enum iio_modifier iio_channel_get_modifier(const struct iio_channel *chn)
{
    return chn->modifier;
}

unsigned int iio_channel_get_attrs_count(const struct iio_channel *chn)
{
    return chn->nattrs;
}

const char *iio_channel_get_attr(const struct iio_channel *chn, unsigned int index)
{
    return list_get(chn->attrs, chn->nattrs, index);
}

const char *iio_channel_find_attr(const struct iio_channel *chn, const char *name)
{
    return list_find(chn->attrs, chn->nattrs, name);
}

const char *iio_channel_attr_get_filename(const struct iio_channel *chn, const char *attr)
{
    for (unsigned i = 0; i < chn->nattrs; i++)
        if (!strcmp(chn->attrs[i].name, attr))
            return chn->attrs[i].filename;
    return 0;
}

ssize_t iio_channel_attr_read(const struct iio_channel *chn, const char *attr, char *dst,
                              size_t len)
{
    return attr_read(chn->dev, chn, ATTR_CHANNEL, attr, dst, len);
}

int iio_channel_attr_read_all(struct iio_channel *chn,
                              int (*cb)(struct iio_channel *chn, const char *attr,
                                        const char *val, size_t len, void *d),
                              void *data)
{
    return read_all(chn->dev, chn, ATTR_CHANNEL, 0, cb, data);
}

int iio_channel_attr_read_bool(const struct iio_channel *chn, const char *attr, bool *val)
{
    return attr_read_bool(chn->dev, chn, ATTR_CHANNEL, attr, val);
}

int iio_channel_attr_read_longlong(const struct iio_channel *chn, const char *attr,
                                   long long *val)
{
    return attr_read_longlong(chn->dev, chn, ATTR_CHANNEL, attr, val);
}

int iio_channel_attr_read_double(const struct iio_channel *chn, const char *attr, double *val)
{
    return attr_read_double(chn->dev, chn, ATTR_CHANNEL, attr, val);
}

ssize_t iio_channel_attr_write(const struct iio_channel *chn, const char *attr, const char *src)
{
    return attr_write(chn->dev, chn, ATTR_CHANNEL, attr, src);
}

ssize_t iio_channel_attr_write_raw(const struct iio_channel *chn, const char *attr,
                                   const void *src, size_t len)
{
    return attr_write_raw(chn->dev, chn, ATTR_CHANNEL, attr, src, len);
}

int iio_channel_attr_write_all(struct iio_channel *chn,
                               ssize_t (*cb)(struct iio_channel *chn, const char *attr,
                                             void *buf, size_t len, void *d),
                               void *data)
{
    return write_all(chn->dev, chn, ATTR_CHANNEL, 0, cb, data);
}

int iio_channel_attr_write_bool(const struct iio_channel *chn, const char *attr, bool val)
{
    return attr_write_bool(chn->dev, chn, ATTR_CHANNEL, attr, val);
}

int iio_channel_attr_write_longlong(const struct iio_channel *chn, const char *attr,
                                    long long val)
{
    return attr_write_longlong(chn->dev, chn, ATTR_CHANNEL, attr, val);
}

int iio_channel_attr_write_double(const struct iio_channel *chn, const char *attr, double val)
{
    return attr_write_double(chn->dev, chn, ATTR_CHANNEL, attr, val);
}

void iio_channel_set_data(struct iio_channel *chn, void *data)
{
    chn->userdata = data;
}

void *iio_channel_get_data(const struct iio_channel *chn)
{
    return chn->userdata;
}

static bool bit(const uint32_t *mask, unsigned n)
{
    return mask[n / 32] >> (n % 32) & 1;
}

void iio_channel_enable(struct iio_channel *chn)
{
    if (chn->scan_element && chn->index >= 0 && chn->dev->mask)
        chn->dev->mask[chn->number / 32] |= 1u << (chn->number % 32);
}

void iio_channel_disable(struct iio_channel *chn)
{
    if (chn->index >= 0 && chn->dev->mask)
        chn->dev->mask[chn->number / 32] &= ~(1u << (chn->number % 32));
}

bool iio_channel_is_enabled(const struct iio_channel *chn)
{
    return chn->index >= 0 && chn->dev->mask && bit(chn->dev->mask, chn->number);
}

long iio_channel_get_index(const struct iio_channel *chn)
{
    return chn->index;
}

const struct iio_data_format *iio_channel_get_data_format(const struct iio_channel *chn)
{
    return &chn->format;
}

/* A sample of len bytes (at most 8) in either byte order, and back. */
static uint64_t load(const uint8_t *p, unsigned len, bool be)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < len; i++)
        v |= (uint64_t)p[be ? len - 1 - i : i] << (8 * i);
    return v;
}

static void store(uint8_t *p, unsigned len, bool be, uint64_t v)
{
    for (unsigned i = 0; i < len; i++)
        p[be ? len - 1 - i : i] = (uint8_t)(v >> (8 * i));
}

void iio_channel_convert(const struct iio_channel *chn, void *dst, const void *src)
{
    const struct iio_data_format *f = &chn->format;
    unsigned len = f->length / 8;
    if (len == 0 || len > 8)
        return;
    for (unsigned r = 0; r < f->repeat; r++) {
        uint64_t v = load((const uint8_t *)src + r * len, len, f->is_be) >> f->shift;
        if (!f->is_fully_defined && f->bits && f->bits < 64) {
            uint64_t top = (uint64_t)1 << (f->bits - 1);
            v &= (top << 1) - 1;
            if (f->is_signed && (v & top))
                v |= ~((top << 1) - 1);     /* (sign-extended to the sample's size) */
        }
        store((uint8_t *)dst + r * len, len, false, v);   /* (the CPUs here are little-endian) */
    }
}

void iio_channel_convert_inverse(const struct iio_channel *chn, void *dst, const void *src)
{
    const struct iio_data_format *f = &chn->format;
    unsigned len = f->length / 8;
    if (len == 0 || len > 8)
        return;
    for (unsigned r = 0; r < f->repeat; r++) {
        uint64_t v = load((const uint8_t *)src + r * len, len, false);
        if (f->bits && f->bits < 64)
            v &= ((uint64_t)1 << f->bits) - 1;
        v <<= f->shift;
        store((uint8_t *)dst + r * len, len, f->is_be, v);
    }
}

/* --- Buffers --- */

/* The bytes of one scan of the channels in mask: each sample aligned to
 * its own size, the whole to the largest (libiio's layout, the kernel's). */
static ssize_t sample_size(const struct iio_device *dev, const uint32_t *mask)
{
    size_t size = 0, largest = 1;
    const struct iio_channel *prev = 0;
    for (unsigned i = 0; i < dev->nchannels; i++) {
        const struct iio_channel *c = dev->channels[i];
        size_t length = c->format.length / 8 * c->format.repeat;
        if (c->index < 0)
            break;
        if (!bit(mask, c->number))
            continue;
        if (prev && c->index == prev->index) {
            prev = c;                       /* (two channels sharing a sample) */
            continue;
        }
        if (length == 0)
            return -EINVAL;
        if (length > largest)
            largest = length;
        if (size % length)
            size += length - size % length;
        size += length;
        prev = c;
    }
    if (size % largest)
        size += largest - size % largest;
    return (ssize_t)size;
}

ssize_t iio_device_get_sample_size(const struct iio_device *dev)
{
    return dev->mask ? sample_size(dev, dev->mask) : 0;
}

static bool device_is_output(const struct iio_device *dev)
{
    for (unsigned i = 0; i < dev->nchannels; i++)
        if (dev->channels[i]->output && dev->channels[i]->scan_element)
            return true;
    return false;
}

struct iio_buffer *iio_device_create_buffer(const struct iio_device *dev, size_t samples_count,
                                            bool cyclic)
{
    ssize_t size = iio_device_get_sample_size(dev);
    if (size < 0)
        return fail((int)size);
    if (size == 0 || samples_count == 0 || samples_count > ((size_t)1 << 32) / (size_t)size)
        return fail(EINVAL);
    if (dev->buffer)
        return fail(EBUSY);
    struct iio_buffer *buf = calloc(1, sizeof(*buf));
    if (!buf)
        return fail(ENOMEM);
    buf->dev = dev;
    buf->fd = -1;
    buf->cyclic = cyclic;
    buf->output = device_is_output(dev);
    buf->samples = samples_count;
    buf->sample_size = (size_t)size;
    buf->length = buf->data_length = (size_t)size * samples_count;
    buf->data = malloc(buf->length);
    buf->mask = calloc(dev->words, sizeof(uint32_t));
    if (!buf->data || !buf->mask) {
        free(buf->data), free(buf->mask), free(buf);
        return fail(ENOMEM);
    }
    memcpy(buf->mask, dev->mask, dev->words * sizeof(uint32_t));
    int ret = iio_net_open(buf);
    if (ret < 0) {
        free(buf->data), free(buf->mask), free(buf);
        return fail(ret);
    }
    ((struct iio_device *)dev)->buffer = buf;
    return buf;
}

void iio_buffer_destroy(struct iio_buffer *buf)
{
    if (!buf)
        return;
    iio_net_close(buf);
    ((struct iio_device *)buf->dev)->buffer = 0;
    free(buf->data), free(buf->mask), free(buf);
}

const struct iio_device *iio_buffer_get_device(const struct iio_buffer *buf)
{
    return buf->dev;
}

ssize_t iio_buffer_refill(struct iio_buffer *buf)
{
    if (buf->cancelled)
        return -EBADF;
    if (buf->output)
        return -EINVAL;
    ssize_t got = iio_net_readbuf(buf, buf->data, buf->length);
    if (got < 0)
        return got;
    buf->data_length = (size_t)got;
    ssize_t size = sample_size(buf->dev, buf->mask);   /* (the server's channels) */
    if (size <= 0)
        return size < 0 ? size : -EIO;
    buf->sample_size = (size_t)size;
    return got;
}

ssize_t iio_buffer_push_partial(struct iio_buffer *buf, size_t samples_count)
{
    if (buf->cancelled)
        return -EBADF;
    size_t length = samples_count * buf->sample_size;
    if (!buf->output || length == 0 || samples_count > buf->samples)
        return -EINVAL;
    if (buf->cyclic && buf->pushed)
        return -EBUSY;                      /* (a cyclic buffer goes once) */
    ssize_t ret = iio_net_writebuf(buf, buf->data, length);
    if (ret >= 0)
        buf->pushed = true;
    buf->data_length = buf->length;
    return ret;
}

ssize_t iio_buffer_push(struct iio_buffer *buf)
{
    return iio_buffer_push_partial(buf, buf->data_length / (buf->sample_size ? buf->sample_size : 1));
}

void iio_buffer_cancel(struct iio_buffer *buf)
{
    __atomic_store_n(&buf->cancelled, true, __ATOMIC_RELEASE);
}

int iio_buffer_set_blocking_mode(struct iio_buffer *buf, bool blocking)
{
    (void)buf;
    return blocking ? 0 : -ENOSYS;
}

int iio_buffer_get_poll_fd(struct iio_buffer *buf)
{
    (void)buf;
    return -ENOSYS;
}

void *iio_buffer_start(const struct iio_buffer *buf)
{
    return buf->data;
}

void *iio_buffer_end(const struct iio_buffer *buf)
{
    return buf->data + buf->data_length;
}

ptrdiff_t iio_buffer_step(const struct iio_buffer *buf)
{
    return (ptrdiff_t)buf->sample_size;
}

void *iio_buffer_first(const struct iio_buffer *buf, const struct iio_channel *chn)
{
    const struct iio_device *dev = buf->dev;
    if (!iio_channel_is_enabled(chn))
        return iio_buffer_end(buf);
    size_t at = 0;
    for (unsigned i = 0; i < dev->nchannels; i++) {
        const struct iio_channel *c = dev->channels[i];
        size_t length = c->format.length / 8 * c->format.repeat;
        if (c->index < 0 || c->index == chn->index)
            break;
        if (!bit(buf->mask, c->number))
            continue;
        if (i > 0 && c->index == dev->channels[i - 1]->index)
            continue;
        if (at % length)
            at += length - at % length;
        at += length;
    }
    size_t length = chn->format.length / 8;
    if (length && at % length)
        at += length - at % length;
    return buf->data + at;
}

ssize_t iio_buffer_foreach_sample(struct iio_buffer *buf,
                                  ssize_t (*callback)(const struct iio_channel *chn, void *src,
                                                      size_t bytes, void *d),
                                  void *data)
{
    const struct iio_device *dev = buf->dev;
    size_t at = 0, end = buf->data_length;
    ssize_t total = 0;
    if (buf->sample_size == 0)
        return -EINVAL;
    while (end - at >= buf->sample_size) {
        size_t start = at;
        for (unsigned i = 0; i < dev->nchannels; i++) {
            const struct iio_channel *c = dev->channels[i];
            size_t length = c->format.length / 8;
            if (c->index < 0)
                break;
            if (!bit(buf->mask, c->number))
                continue;
            if ((at - start) % length)
                at += length - (at - start) % length;
            if (bit(dev->mask, c->number)) {
                ssize_t r = callback(c, buf->data + at, length, data);
                if (r < 0)
                    return r;
                total += r;
            }
            if (i == dev->nchannels - 1 || dev->channels[i + 1]->index != c->index)
                at += length * c->format.repeat;
        }
        at = start + buf->sample_size;      /* (the scan's padding) */
    }
    return total;
}

void iio_buffer_set_data(struct iio_buffer *buf, void *data)
{
    buf->userdata = data;
}

void *iio_buffer_get_data(const struct iio_buffer *buf)
{
    return buf->userdata;
}

size_t iio_channel_read_raw(const struct iio_channel *chn, struct iio_buffer *buffer, void *dst,
                            size_t len)
{
    size_t length = chn->format.length / 8 * chn->format.repeat, done = 0;
    char *end = iio_buffer_end(buffer);
    for (char *p = iio_buffer_first(buffer, chn); p + length <= end && done + length <= len;
         p += buffer->sample_size, done += length)
        memcpy((char *)dst + done, p, length);
    return done;
}

size_t iio_channel_read(const struct iio_channel *chn, struct iio_buffer *buffer, void *dst,
                        size_t len)
{
    size_t length = chn->format.length / 8 * chn->format.repeat, done = 0;
    char *end = iio_buffer_end(buffer);
    for (char *p = iio_buffer_first(buffer, chn); p + length <= end && done + length <= len;
         p += buffer->sample_size, done += length)
        iio_channel_convert(chn, (char *)dst + done, p);
    return done;
}

size_t iio_channel_write_raw(const struct iio_channel *chn, struct iio_buffer *buffer,
                             const void *src, size_t len)
{
    size_t length = chn->format.length / 8 * chn->format.repeat, done = 0;
    char *end = iio_buffer_end(buffer);
    for (char *p = iio_buffer_first(buffer, chn); p + length <= end && done + length <= len;
         p += buffer->sample_size, done += length)
        memcpy(p, (const char *)src + done, length);
    return done;
}

size_t iio_channel_write(const struct iio_channel *chn, struct iio_buffer *buffer,
                         const void *src, size_t len)
{
    size_t length = chn->format.length / 8 * chn->format.repeat, done = 0;
    char *end = iio_buffer_end(buffer);
    for (char *p = iio_buffer_first(buffer, chn); p + length <= end && done + length <= len;
         p += buffer->sample_size, done += length)
        iio_channel_convert_inverse(chn, p, (const char *)src + done);
    return done;
}
