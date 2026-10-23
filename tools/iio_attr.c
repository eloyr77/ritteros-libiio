/*
 * iio_attr: read or write one attribute of an IIO context, as
 * libiio's iio_attr does (its main forms; names match exactly, no
 * patterns). Without an attribute it lists them all with their values; a
 * value writes it, then reads it back.
 *
 *   iio_attr -u URI -c [-i|-o] DEVICE CHANNEL [ATTR [VALUE]]
 *   iio_attr -u URI -d DEVICE [ATTR [VALUE]]           (-B buffer, -D debug)
 *   iio_attr -u URI -C [ATTR]                          (context attributes)
 */
#include <errno.h>
#include <iio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ritteros.h"

PROGRAM_HELP("iio_attr -u URI [-q] -c [-i|-o] DEV CHN [ATTR [VALUE]] | -d|-B|-D DEV [ATTR [VALUE]] | -C [ATTR]",
             "read or write an IIO attribute (a Pluto's LO: -c ad9361-phy RX_LO frequency)");

#define VALUE_MAX 0x10000

enum kind { DEVICE, BUFFER, DEBUG, CHANNEL };
static const char *const kind_text[] = { "attr", "buffer attr", "debug attr", "attr" };

static bool quiet;
static char *buf;

static void usage(void)
{
    printf("usage: iio_attr -u URI [-q] -c [-i|-o] DEVICE CHANNEL [ATTR [VALUE]]\n"
           "       iio_attr -u URI [-q] -d|-B|-D DEVICE [ATTR [VALUE]]\n"
           "       iio_attr -u URI [-q] -C [ATTR]\n"
           "  -c  a channel's attributes (-i input, -o output: when both have the ID)\n"
           "  -d  a device's, -B its buffer's, -D its debug attributes\n"
           "  -C  the context's attributes\n"
           "  -q  print the value only\n");
}

static const char *device_text(const struct iio_device *dev)
{
    const char *l = iio_device_get_label(dev), *n = iio_device_get_name(dev);
    return l ? l : n ? n : iio_device_get_id(dev);
}

static const char *error_text(ssize_t ret)
{
    static char text[128];
    iio_strerror((int)-ret, text, sizeof(text));
    return text;
}

static ssize_t read_one(struct iio_device *dev, struct iio_channel *ch, enum kind k,
                        const char *attr)
{
    switch (k) {
    case CHANNEL:
        return iio_channel_attr_read(ch, attr, buf, VALUE_MAX);
    case BUFFER:
        return iio_device_buffer_attr_read(dev, attr, buf, VALUE_MAX);
    case DEBUG:
        return iio_device_debug_attr_read(dev, attr, buf, VALUE_MAX);
    default:
        return iio_device_attr_read(dev, attr, buf, VALUE_MAX);
    }
}

static ssize_t write_one(struct iio_device *dev, struct iio_channel *ch, enum kind k,
                         const char *attr, const char *v)
{
    switch (k) {
    case CHANNEL:
        return iio_channel_attr_write(ch, attr, v);
    case BUFFER:
        return iio_device_buffer_attr_write(dev, attr, v);
    case DEBUG:
        return iio_device_debug_attr_write(dev, attr, v);
    default:
        return iio_device_attr_write(dev, attr, v);
    }
}

static void show(struct iio_device *dev, struct iio_channel *ch, enum kind k, const char *attr)
{
    ssize_t ret = read_one(dev, ch, k, attr);
    if (!quiet) {
        printf("%s '%s'", iio_device_is_trigger(dev) ? "trig" : "dev", device_text(dev));
        if (k == CHANNEL) {
            printf(", channel '%s' (%s), ", iio_channel_get_id(ch),
                   iio_channel_is_output(ch) ? "output" : "input");
            if (iio_channel_get_name(ch))
                printf("id '%s', ", iio_channel_get_name(ch));
            printf("attr '%s', ", attr);
        } else {
            printf(", %s '%s', ", kind_text[k], attr);
        }
    }
    if (ret < 0)
        printf("ERROR: %s\n", error_text(ret));
    else if (quiet)
        printf("%s\n", buf);
    else
        printf("value '%s'\n", buf);
}

/* Read ATTR, or write VALUE and read it back, or show every attribute. */
static int attrs(struct iio_device *dev, struct iio_channel *ch, enum kind k, const char *attr,
                 const char *v)
{
    if (!attr) {
        unsigned n = k == CHANNEL ? iio_channel_get_attrs_count(ch)
                   : k == BUFFER  ? iio_device_get_buffer_attrs_count(dev)
                   : k == DEBUG   ? iio_device_get_debug_attrs_count(dev)
                                  : iio_device_get_attrs_count(dev);
        if (!quiet)
            printf("found %u %ss\n", n, kind_text[k]);
        for (unsigned i = 0; i < n; i++)
            show(dev, ch, k,
                 k == CHANNEL ? iio_channel_get_attr(ch, i)
                 : k == BUFFER ? iio_device_get_buffer_attr(dev, i)
                 : k == DEBUG  ? iio_device_get_debug_attr(dev, i) : iio_device_get_attr(dev, i));
        return 0;
    }
    if (v) {
        ssize_t ret = write_one(dev, ch, k, attr, v);
        if (ret < 0) {
            printf("ERROR: %s while writing '%s' with '%s'\n", error_text(ret), attr, v);
            return 1;
        }
        if (!quiet)
            printf("wrote %ld bytes to %s\n", (long)ret, attr);
    }
    show(dev, ch, k, attr);
    return 0;
}

int main(int argc, char **argv)
{
    const char *uri = 0;
    int kind = -1;
    int direction = -1;                     /* -1 either, 0 input, 1 output */
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        const char *o = argv[i];
        if (!strcmp(o, "-h") || !strcmp(o, "--help")) {
            usage();
            return 0;
        } else if (!strcmp(o, "-u") && i + 1 < argc) {
            uri = argv[++i];
        } else if (!strcmp(o, "-q")) {
            quiet = true;
        } else if (!strcmp(o, "-i") || !strcmp(o, "-o")) {
            direction = o[1] == 'o';
        } else if (!strcmp(o, "-c") || !strcmp(o, "-d") || !strcmp(o, "-B") || !strcmp(o, "-D") ||
                   !strcmp(o, "-C")) {
            int k = o[1] == 'c' ? CHANNEL : o[1] == 'd' ? DEVICE : o[1] == 'B' ? BUFFER
                  : o[1] == 'D' ? DEBUG : 4;
            if (kind >= 0 && kind != k) {
                printf("iio_attr: the options -d, -c, -C, -B and -D are exclusive\n");
                return 2;
            }
            kind = k;
        } else {
            usage();
            return 2;
        }
    }
    if (!uri || kind < 0) {
        if (!uri)
            printf("iio_attr: give the context with -u URI (there are no local devices)\n");
        else
            printf("iio_attr: give one of -d, -c, -C, -B or -D\n");
        usage();
        return 2;
    }
    char **rest = argv + i;
    int left = argc - i;
    int need = kind == CHANNEL ? 2 : kind == 4 ? 0 : 1;
    if (left < need || left > need + (kind == 4 ? 1 : 2)) {
        usage();
        return 2;
    }

    struct iio_context *ctx = iio_create_context_from_uri(uri);
    if (!ctx) {
        printf("iio_attr: %s: %s\n", uri, strerror(errno));
        return 1;
    }
    buf = malloc(VALUE_MAX);
    if (!buf)
        return 1;
    int ret = 0;
    if (kind == 4) {
        unsigned n = iio_context_get_attrs_count(ctx);
        if (!quiet && left == 0)
            printf("IIO context with %u attributes:\n", n);
        bool found = false;
        for (unsigned k = 0; k < n; k++) {
            const char *key, *val;
            iio_context_get_attr(ctx, k, &key, &val);
            if (left == 1 && strcmp(key, rest[0]))
                continue;
            found = true;
            if (quiet)
                printf("%s\n", val);
            else
                printf("%s: %s\n", key, val);
        }
        if (left == 1 && !found) {
            printf("iio_attr: the context has no attribute '%s'\n", rest[0]);
            ret = 1;
        }
    } else {
        struct iio_device *dev = iio_context_find_device(ctx, rest[0]);
        struct iio_channel *ch = 0;
        if (!dev) {
            printf("iio_attr: no device '%s'\n", rest[0]);
            ret = 1;
        } else if (kind == CHANNEL) {
            if (direction != 0)
                ch = iio_device_find_channel(dev, rest[1], true);
            if (!ch && direction != 1)
                ch = iio_device_find_channel(dev, rest[1], false);
            if (!ch) {
                printf("iio_attr: device '%s' has no channel '%s'\n", rest[0], rest[1]);
                ret = 1;
            }
        }
        if (ret == 0) {
            int at = kind == CHANNEL ? 2 : 1;
            const char *attr = left > at ? rest[at] : 0, *v = left > at + 1 ? rest[at + 1] : 0;
            bool known = !attr || (kind == CHANNEL ? iio_channel_find_attr(ch, attr) != 0
                                   : kind == BUFFER ? iio_device_find_buffer_attr(dev, attr) != 0
                                   : kind == DEBUG  ? iio_device_find_debug_attr(dev, attr) != 0
                                                    : iio_device_find_attr(dev, attr) != 0);
            if (!known) {
                printf("iio_attr: '%s' has no %s '%s'\n", kind == CHANNEL ? rest[1] : rest[0],
                       kind_text[kind], attr);
                ret = 1;
            } else {
                ret = attrs(dev, ch, (enum kind)kind, attr, v);
            }
        }
    }
    free(buf);
    iio_context_destroy(ctx);
    return ret;
}
