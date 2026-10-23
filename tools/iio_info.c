/*
 * iio_info: everything an IIO context has -- its attributes, its
 * devices, their channels and every attribute's value -- as libiio's own
 * iio_info prints it. The context comes from -u URI ("ip:192.168.2.1"),
 * -n HOST or -x FILE (there are no local devices).
 */
#include <errno.h>
#include <iio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ritteros.h"

PROGRAM_HELP("iio_info -u URI | -n HOST | -x FILE [-T MS]",
             "list an IIO context (a Pluto at ip:192.168.2.1): its devices, channels and attributes");

#define VALUE_MAX 0x10000

static void usage(void)
{
    printf("usage: iio_info -u URI | -n HOST | -x FILE [-T TIMEOUT_MS]\n"
           "  -u URI    the context: ip:HOST[:PORT] or xml:FILE\n"
           "  -n HOST   the same as -u ip:HOST\n"
           "  -x FILE   the same as -u xml:FILE\n"
           "  -T MS     how long to wait for the server (5000)\n");
}

static void value(ssize_t ret, char *buf)
{
    if (ret > 0) {
        printf("value: %s\n", buf);
    } else {
        iio_strerror((int)-ret, buf, VALUE_MAX);
        printf("ERROR: %s\n", ret == 0 ? "empty" : buf);
    }
}

static bool buffer_capable(const struct iio_device *dev)
{
    for (unsigned i = 0; i < iio_device_get_channels_count(dev); i++)
        if (iio_channel_is_scan_element(iio_device_get_channel(dev, i)))
            return true;
    return false;
}

static void show_channel(struct iio_channel *ch, char *buf)
{
    const char *name = iio_channel_get_name(ch);
    printf("\t\t\t%s: %s (%s", iio_channel_get_id(ch), name ? name : "",
           iio_channel_is_output(ch) ? "output" : "input");
    if (iio_channel_get_type(ch) == IIO_CHAN_TYPE_UNKNOWN)
        printf(", WARN:iio_channel_get_type()=UNKNOWN");
    if (iio_channel_is_scan_element(ch)) {
        const struct iio_data_format *f = iio_channel_get_data_format(ch);
        char sign = f->is_signed ? 's' : 'u', repeat[12] = "";
        if (f->is_fully_defined)
            sign = (char)(sign - 'a' + 'A');
        if (f->repeat > 1)
            snprintf(repeat, sizeof(repeat), "X%u", f->repeat);
        printf(", index: %ld, format: %ce:%c%u/%u%s>>%u)\n", iio_channel_get_index(ch),
               f->is_be ? 'b' : 'l', sign, f->bits, f->length, repeat, f->shift);
    } else {
        printf(")\n");
    }
    unsigned n = iio_channel_get_attrs_count(ch);
    if (!n)
        return;
    printf("\t\t\t%u channel-specific attributes found:\n", n);
    for (unsigned k = 0; k < n; k++) {
        const char *attr = iio_channel_get_attr(ch, k);
        printf("\t\t\t\tattr %2u: %s ", k, attr);
        value(iio_channel_attr_read(ch, attr, buf, VALUE_MAX), buf);
    }
}

int main(int argc, char **argv)
{
    const char *uri = 0;
    char made[300];
    unsigned timeout = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage();
            return 0;
        } else if (i + 1 < argc && !strcmp(argv[i], "-u")) {
            uri = argv[++i];
        } else if (i + 1 < argc && !strcmp(argv[i], "-n")) {
            snprintf(made, sizeof(made), "ip:%s", argv[++i]);
            uri = made;
        } else if (i + 1 < argc && !strcmp(argv[i], "-x")) {
            snprintf(made, sizeof(made), "xml:%s", argv[++i]);
            uri = made;
        } else if (i + 1 < argc && !strcmp(argv[i], "-T")) {
            timeout = (unsigned)strtoul(argv[++i], 0, 10);
        } else {
            usage();
            return 2;
        }
    }
    if (!uri) {
        printf("iio_info: give the context with -u URI (there are no local devices)\n");
        usage();
        return 2;
    }

    unsigned major, minor;
    char tag[8];
    iio_library_get_version(&major, &minor, tag);
    printf("Library version: %u.%u (git tag: %s)\n", major, minor, tag);
    printf("Compiled with backends:");
    for (unsigned i = 0; i < iio_get_backends_count(); i++)
        printf(" %s", iio_get_backend(i));
    printf("\n");

    struct iio_context *ctx = iio_create_context_from_uri(uri);
    if (!ctx) {
        printf("iio_info: %s: %s\n", uri, strerror(errno));
        return 1;
    }
    if (timeout && iio_context_set_timeout(ctx, timeout) < 0)
        printf("iio_info: the timeout was not taken\n");
    char *buf = malloc(VALUE_MAX);
    if (!buf) {
        printf("iio_info: out of memory\n");
        return 1;
    }
    printf("IIO context created with %s backend.\n", iio_context_get_name(ctx));
    if (iio_context_get_version(ctx, &major, &minor, tag) == 0)
        printf("Backend version: %u.%u (git tag: %s)\n", major, minor, tag);
    printf("Backend description string: %s\n", iio_context_get_description(ctx));
    unsigned n = iio_context_get_attrs_count(ctx);
    if (n)
        printf("IIO context has %u attributes:\n", n);
    for (unsigned i = 0; i < n; i++) {
        const char *key, *val;
        if (iio_context_get_attr(ctx, i, &key, &val) == 0)
            printf("\t%s: %s\n", key, val);
    }
    n = iio_context_get_devices_count(ctx);
    printf("IIO context has %u devices:\n", n);
    for (unsigned i = 0; i < n; i++) {
        struct iio_device *dev = iio_context_get_device(ctx, i);
        const char *name = iio_device_get_name(dev), *label = iio_device_get_label(dev);
        printf("\t%s:", iio_device_get_id(dev));
        if (name)
            printf(" %s", name);
        if (label)
            printf(" (label: %s)", label);
        if (buffer_capable(dev))
            printf(" (buffer capable)");
        printf("\n");
        unsigned channels = iio_device_get_channels_count(dev);
        printf("\t\t%u channels found:\n", channels);
        for (unsigned j = 0; j < channels; j++)
            show_channel(iio_device_get_channel(dev, j), buf);
        unsigned count = iio_device_get_attrs_count(dev);
        if (count) {
            printf("\t\t%u device-specific attributes found:\n", count);
            for (unsigned j = 0; j < count; j++) {
                const char *attr = iio_device_get_attr(dev, j);
                printf("\t\t\t\tattr %2u: %s ", j, attr);
                value(iio_device_attr_read(dev, attr, buf, VALUE_MAX), buf);
            }
        }
        count = iio_device_get_buffer_attrs_count(dev);
        if (count) {
            printf("\t\t%u buffer-specific attributes found:\n", count);
            for (unsigned j = 0; j < count; j++) {
                const char *attr = iio_device_get_buffer_attr(dev, j);
                printf("\t\t\t\tattr %2u: %s ", j, attr);
                value(iio_device_buffer_attr_read(dev, attr, buf, VALUE_MAX), buf);
            }
        }
        count = iio_device_get_debug_attrs_count(dev);
        if (count) {
            printf("\t\t%u debug attributes found:\n", count);
            for (unsigned j = 0; j < count; j++) {
                const char *attr = iio_device_get_debug_attr(dev, j);
                printf("\t\t\t\tdebug attr %2u: %s ", j, attr);
                value(iio_device_debug_attr_read(dev, attr, buf, VALUE_MAX), buf);
            }
        }
        const struct iio_device *trig;
        int ret = iio_device_get_trigger(dev, &trig);
        if (ret == 0) {
            if (!trig)
                printf("\t\tNo trigger assigned to device\n");
            else
                printf("\t\tCurrent trigger: %s(%s)\n", iio_device_get_id(trig),
                       iio_device_get_name(trig) ? iio_device_get_name(trig) : "");
        } else if (ret == -ENOENT || ret == -ENOSYS) {
            printf("\t\tNo trigger on this device\n");
        } else {
            iio_strerror(-ret, buf, VALUE_MAX);
            printf("ERROR: checking for trigger : %s\n", buf);
        }
    }
    free(buf);
    iio_context_destroy(ctx);
    return 0;
}
