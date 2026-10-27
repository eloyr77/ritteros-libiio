/*
 * Test: libiio against tests/testiiod.py, the pretend Pluto. iiotest URI.
 * Every line it prints must be what `testiiod.py expect` says (its model of
 * the pretend Pluto); the samples it sends are checked in the server's log.
 */
#include <errno.h>
#include <iio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ritteros.h"

#define T "iiotest: "

static void hex(const void *data, size_t n, char out[65])
{
    struct sha256 s;
    uint8_t d[SHA256_SIZE];
    sha256_init(&s);
    sha256_update(&s, data, n);
    sha256_final(&s, d);
    for (int i = 0; i < 32; i++)
        snprintf(out + 2 * i, 3, "%02x", d[i]);
}

static const char *err(int e)
{
    static char text[64];
    iio_strerror(e, text, sizeof(text));
    return text;
}

static int show_attr(struct iio_channel *chn, const char *attr, const char *val, size_t len,
                     void *d)
{
    (void)chn;
    (*(int *)d)++;
    printf(T "  %s = '%s' (%zu)\n", attr, val, len);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf(T "usage: iiotest URI\n");
        return 2;
    }
    unsigned major, minor;
    char tag[8];
    iio_library_get_version(&major, &minor, tag);
    printf(T "library %u.%u (%s), backends %u: %s %s, has ip %d usb %d\n", major, minor, tag,
           iio_get_backends_count(), iio_get_backend(0), iio_get_backend(1), iio_has_backend("ip"),
           iio_has_backend("usb"));

    struct iio_context *bad = iio_create_context_from_uri("usb:1.2.3");
    printf(T "usb: %s, errno %d\n", bad ? "made" : "refused", bad ? 0 : errno);

    struct iio_context *ctx = iio_create_context_from_uri(argv[1]);
    if (!ctx) {
        printf(T "no context: %s\n", err(errno));
        return 1;
    }
    iio_context_get_version(ctx, &major, &minor, tag);
    printf(T "context '%s', backend %u.%u (%s), %u devices\n", iio_context_get_name(ctx), major,
           minor, tag, iio_context_get_devices_count(ctx));
    printf(T "description '%s'\n", iio_context_get_description(ctx));
    printf(T "%u attributes, hw_model '%s', ip,ip-addr '%s'\n", iio_context_get_attrs_count(ctx),
           iio_context_get_attr_value(ctx, "hw_model"),
           iio_context_get_attr_value(ctx, "ip,ip-addr"));
    for (unsigned i = 0; i < iio_context_get_devices_count(ctx); i++) {
        struct iio_device *d = iio_context_get_device(ctx, i);
        printf(T "%s '%s': %u channels, %u attributes, %u buffer, %u debug, trigger %d hwmon %d\n",
               iio_device_get_id(d), iio_device_get_name(d) ? iio_device_get_name(d) : "",
               iio_device_get_channels_count(d), iio_device_get_attrs_count(d),
               iio_device_get_buffer_attrs_count(d), iio_device_get_debug_attrs_count(d),
               iio_device_is_trigger(d), iio_device_is_hwmon(d));
    }

    struct iio_device *phy = iio_context_find_device(ctx, "ad9361-phy");
    struct iio_device *rx = iio_context_find_device(ctx, "cf-ad9361-lpc");
    struct iio_device *tx = iio_context_find_device(ctx, "iio:device2");
    struct iio_device *hw = iio_context_find_device(ctx, "hwmon0");
    if (!phy || !rx || !tx || !hw) {
        printf(T "devices missing\n");
        return 1;
    }
    for (unsigned i = 0; i < iio_device_get_channels_count(rx); i++) {
        struct iio_channel *c = iio_device_get_channel(rx, i);
        const struct iio_data_format *f = iio_channel_get_data_format(c);
        printf(T "rx channel %u: %s index %ld, %u/%u>>%u signed %d be %d full %d repeat %u\n", i,
               iio_channel_get_id(c), iio_channel_get_index(c), f->bits, f->length, f->shift,
               f->is_signed, f->is_be, f->is_fully_defined, f->repeat);
    }

    struct iio_channel *lo = iio_device_find_channel(phy, "RX_LO", true);
    struct iio_channel *gain = iio_device_find_channel(phy, "voltage0", false);
    struct iio_channel *temp = iio_device_find_channel(hw, "temp1", false);
    printf(T "RX_LO is %s (%s), type %d modifier %d; voltage0 type %d; hwmon temp1 type %d\n",
           iio_channel_get_id(lo), iio_channel_is_output(lo) ? "output" : "input",
           iio_channel_get_type(lo), iio_channel_get_modifier(lo), iio_channel_get_type(gain),
           iio_channel_get_type(temp));
    printf(T "RX_LO frequency file %s\n", iio_channel_attr_get_filename(lo, "frequency"));
    struct iio_channel *found = 0;
    const char *which = 0;
    int r = iio_device_identify_filename(phy, "out_altvoltage1_TX_LO_frequency", &found, &which);
    printf(T "identify: %d %s %s\n", r, found ? iio_channel_get_id(found) : "-", which ? which : "-");
    r = iio_device_identify_filename(phy, "ensm_mode", &found, &which);
    printf(T "identify: %d %s %s\n", r, found ? iio_channel_get_id(found) : "-", which ? which : "-");

    long long f;
    r = iio_channel_attr_read_longlong(lo, "frequency", &f);
    printf(T "LO %d %lld\n", r, f);
    r = iio_channel_attr_write_longlong(lo, "frequency", 100000000);
    iio_channel_attr_read_longlong(lo, "frequency", &f);
    printf(T "LO set %d, now %lld\n", r, f);
    double g;
    r = iio_channel_attr_read_double(gain, "hardwaregain", &g);
    printf(T "gain %d %.2f\n", r, g);
    struct iio_channel *txgain = iio_device_find_channel(phy, "voltage0", true);
    r = iio_channel_attr_write_double(txgain, "hardwaregain", -20.25);
    iio_channel_attr_read_double(txgain, "hardwaregain", &g);
    printf(T "tx gain set %d, now %.2f\n", r, g);
    bool b = true;
    r = iio_channel_attr_read_bool(lo, "powerdown", &b);
    printf(T "powerdown %d %d", r, b);
    iio_channel_attr_write_bool(lo, "powerdown", true);
    iio_channel_attr_read_bool(lo, "powerdown", &b);
    printf(", now %d\n", b);
    char text[256];
    ssize_t n = iio_channel_attr_write(gain, "gain_control_mode", "manual");
    iio_channel_attr_read(gain, "gain_control_mode", text, sizeof(text));
    printf(T "gain mode wrote %zd, now '%s'\n", n, text);
    n = iio_device_attr_read(phy, "ensm_mode", text, sizeof(text));
    printf(T "ensm_mode %zd '%s'\n", n, text);
    n = iio_device_attr_read(phy, "ensm_mode", text, 3);
    printf(T "ensm_mode in 3 bytes: %s\n", err((int)n));
    n = iio_device_attr_read(phy, "calib_mode", text, sizeof(text));
    printf(T "after it, calib_mode %zd '%s'\n", n, text);
    /* An answer that comes after the timeout must not be taken for the next
     * question's: the library opens a new connection (and gives the server
     * the shortened timeout again there: check.sh looks). */
    iio_context_set_timeout(ctx, 500);
    n = iio_device_debug_attr_read(phy, "slow_answer", text, sizeof(text));
    printf(T "slow_answer in 500 ms: timed out %d", n == -ETIMEDOUT);
    n = iio_device_attr_read(phy, "calib_mode", text, sizeof(text));
    printf("; after it, calib_mode %zd '%s'", n, text);
    n = iio_device_attr_read(phy, "xo_correction", text, sizeof(text));
    printf(", xo_correction %zd '%s'\n", n, text);
    iio_context_set_timeout(ctx, 5000);
    n = iio_channel_attr_read(lo, "fastlock_recall", text, sizeof(text));
    printf(T "fastlock_recall: %s\n", err((int)n));
    n = iio_channel_attr_read(lo, "nonexistent", text, sizeof(text));
    printf(T "nonexistent: %s\n", err((int)n));
    n = iio_device_buffer_attr_read(rx, "watermark", text, sizeof(text));
    printf(T "rx buffer watermark %zd '%s'\n", n, text);
    int count = 0;
    r = iio_channel_attr_read_all(lo, show_attr, &count);
    printf(T "read_all %d: %d values\n", r, count);

    uint32_t value = 0;
    r = iio_device_reg_write(phy, 0x37, 0x5A);
    int r2 = iio_device_reg_read(phy, 0x37, &value);
    printf(T "register 0x37: write %d read %d value 0x%X\n", r, r2, (unsigned)value);

    const struct iio_device *trig = (void *)1;
    r = iio_device_get_trigger(rx, &trig);
    printf(T "trigger %d %s", r, trig ? iio_device_get_name(trig) : "none");
    r = iio_device_set_trigger(rx, iio_context_find_device(ctx, "trigger0"));
    r2 = iio_device_get_trigger(rx, &trig);
    printf(", set %d, get %d %s\n", r, r2, trig ? iio_device_get_name(trig) : "none");
    printf(T "kernel buffers %d, timeout %d\n", iio_device_set_kernel_buffers_count(rx, 4),
           iio_context_set_timeout(ctx, 3000));

    /* Sample conversion: S12 in 16 bits, little-endian. */
    struct iio_channel *i0 = iio_device_find_channel(rx, "voltage0", false);
    struct iio_channel *q0 = iio_device_find_channel(rx, "voltage1", false);
    uint16_t raws[] = { 0x0FFF, 0x0800, 0x07FF, 0xF800, 0x0001 };
    printf(T "convert");
    for (unsigned i = 0; i < sizeof(raws) / sizeof(raws[0]); i++) {
        int16_t v;
        iio_channel_convert(i0, &v, &raws[i]);
        printf(" %04x:%d", raws[i], v);
    }
    int16_t minus1 = -1;
    uint16_t back;
    iio_channel_convert_inverse(i0, &back, &minus1);
    printf(", -1 back %04x\n", back);
    struct iio_device *xadc = iio_context_find_device(ctx, "xadc");
    struct iio_channel *vs = iio_device_find_channel(xadc, "vccint", false);
    struct iio_channel *tu = iio_device_find_channel(xadc, "temp0", false);
    uint8_t le[2] = { 0xF0, 0xFF }, le2[2] = { 0x10, 0x7F }, be[2] = { 0xAB, 0xCD };
    int16_t sv, sv2;
    uint16_t uv;
    iio_channel_convert(vs, &sv, le);
    iio_channel_convert(vs, &sv2, le2);
    iio_channel_convert(tu, &uv, be);
    int16_t minus5 = -5;
    uint16_t five = 0x0ABC;
    uint8_t lb[2], bb[2];
    iio_channel_convert_inverse(vs, lb, &minus5);
    iio_channel_convert_inverse(tu, bb, &five);
    printf(T "convert s12>>4 fff0:%d 7f10:%d, be u12>>4 abcd:%u; back -5 %02x%02x 0abc %02x%02x\n",
           sv, sv2, uv, lb[0], lb[1], bb[0], bb[1]);

    /* Receiving. */
    printf(T "sample size before %zd", iio_device_get_sample_size(rx));
    struct iio_buffer *none = iio_device_create_buffer(rx, 16, false);
    printf(", buffer with no channels: %s\n", none ? "made" : err(errno));
    iio_channel_enable(i0);
    iio_channel_enable(q0);
    printf(T "enabled %d %d, sample size %zd\n", iio_channel_is_enabled(i0),
           iio_channel_is_enabled(q0), iio_device_get_sample_size(rx));
    struct iio_buffer *buf = iio_device_create_buffer(rx, 4096, false);
    if (!buf) {
        printf(T "no rx buffer: %s\n", err(errno));
        return 1;
    }
    struct iio_buffer *second = iio_device_create_buffer(rx, 16, false);
    printf(T "second buffer: %s\n", second ? "made" : err(errno));
    n = iio_buffer_refill(buf);
    int16_t *is = malloc(4096 * 2), *qs = malloc(4096 * 2);
    size_t ni = iio_channel_read(i0, buf, is, 4096 * 2);
    size_t nq = iio_channel_read(q0, buf, qs, 4096 * 2);
    char hi[65], hq[65];
    hex(is, ni, hi);
    hex(qs, nq, hq);
    printf(T "refill %zd, first I %d Q %d at offsets %ld %ld\n", n, is[0], qs[0],
           (long)((char *)iio_buffer_first(buf, i0) - (char *)iio_buffer_start(buf)),
           (long)((char *)iio_buffer_first(buf, q0) - (char *)iio_buffer_start(buf)));
    printf(T "rx %zu scans, step %ld, I sha256 %s\n", ni / 2, (long)iio_buffer_step(buf), hi);
    printf(T "rx Q sha256 %s\n", hq);
    n = iio_buffer_refill(buf);
    iio_channel_read(i0, buf, is, 2);
    iio_channel_read(q0, buf, qs, 2);
    printf(T "refill again %zd, first I %d Q %d\n", n, is[0], qs[0]);
    iio_buffer_destroy(buf);

    iio_channel_disable(q0);
    buf = iio_device_create_buffer(rx, 8, false);
    n = buf ? iio_buffer_refill(buf) : -1;
    int16_t only[8];
    size_t got = buf ? iio_channel_read(i0, buf, only, sizeof(only)) : 0;
    printf(T "I alone: size %zd, refill %zd, %zu bytes, %d %d, step %ld\n",
           iio_device_get_sample_size(rx), n, got, only[0], only[1],
           buf ? (long)iio_buffer_step(buf) : 0);
    iio_buffer_destroy(buf);

    /* Sending. */
    struct iio_channel *ti = iio_device_find_channel(tx, "voltage0", true);
    struct iio_channel *tq = iio_device_find_channel(tx, "voltage1", true);
    iio_channel_enable(ti);
    iio_channel_enable(tq);
    buf = iio_device_create_buffer(tx, 1024, false);
    if (!buf) {
        printf(T "no tx buffer: %s\n", err(errno));
        return 1;
    }
    int16_t wave[1024];
    for (int k = 0; k < 1024; k++)
        wave[k] = (int16_t)(k * 31 - 16000);
    size_t wi = iio_channel_write(ti, buf, wave, sizeof(wave));
    for (int k = 0; k < 1024; k++)
        wave[k] = (int16_t)(-wave[k]);
    size_t wq = iio_channel_write(tq, buf, wave, sizeof(wave));
    char ht[65];
    hex(iio_buffer_start(buf), (size_t)((char *)iio_buffer_end(buf) - (char *)iio_buffer_start(buf)),
        ht);
    n = iio_buffer_push(buf);
    printf(T "tx wrote %zu %zu, pushed %zd, sha256 %s\n", wi, wq, n, ht);
    hex(iio_buffer_start(buf), 100 * 4, ht);
    n = iio_buffer_push_partial(buf, 100);
    printf(T "tx partial %zd, sha256 %s\n", n, ht);
    printf(T "tx refill: %s\n", err((int)iio_buffer_refill(buf)));
    iio_buffer_destroy(buf);
    buf = iio_device_create_buffer(tx, 256, true);
    n = buf ? iio_buffer_push(buf) : -1;
    ssize_t again = buf ? iio_buffer_push(buf) : -1;
    printf(T "cyclic push %zd, again: %s\n", n, err((int)again));
    if (buf) {
        iio_buffer_cancel(buf);
        printf(T "after cancel: %s\n", err((int)iio_buffer_push(buf)));
        iio_buffer_destroy(buf);
    }

    /* The description again, and a second connection. */
    const char *xml = iio_context_get_xml(ctx);
    struct iio_context *copy = iio_create_xml_context_mem(xml, strlen(xml));
    struct iio_context *clone = iio_context_clone(ctx);
    printf(T "xml copy '%s' %u devices; clone '%s' %u devices, LO %s\n",
           copy ? iio_context_get_name(copy) : "-", copy ? iio_context_get_devices_count(copy) : 0,
           clone ? iio_context_get_name(clone) : "-",
           clone ? iio_context_get_devices_count(clone) : 0,
           clone && iio_channel_attr_read(iio_device_find_channel(iio_context_find_device(clone,
                       "ad9361-phy"), "altvoltage0", true), "frequency", text, sizeof(text)) > 0
               ? text : "-");
    n = copy ? iio_device_attr_read(iio_context_get_device(copy, 0), "ensm_mode", text, 64) : 0;
    printf(T "xml copy read: %s\n", err((int)n));
    struct iio_context *broken = iio_create_xml_context_mem("<context><device id='x'>", 24);
    printf(T "broken xml: %s\n", broken ? "made" : err(errno));
    iio_context_destroy(copy);
    iio_context_destroy(clone);
    iio_context_destroy(ctx);
    printf(T "done\n");
    return 0;
}
