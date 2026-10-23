/*
 * Industrial I/O for RitterOS: sensors, converters and radios such as Analog
 * Devices' ADALM-Pluto, reached through an iiod server over the network, in
 * libiio.a (a static library, built with RitterOS's SDK). The interface is
 * libiio's classic one (version 0.26: the same names, types and
 * behaviour), so code written for libiio builds here;
 * the code is our own, from iiod's text protocol (TCP port 30431).
 *
 *     struct iio_context *ctx = iio_create_context_from_uri("ip:192.168.2.1");
 *     struct iio_device *phy = iio_context_find_device(ctx, "ad9361-phy");
 *     struct iio_channel *lo = iio_device_find_channel(phy, "altvoltage0", true);
 *     iio_channel_attr_write_longlong(lo, "frequency", 100000000);   // RX LO
 *
 *     struct iio_device *rx = iio_context_find_device(ctx, "cf-ad9361-lpc");
 *     iio_channel_enable(iio_device_find_channel(rx, "voltage0", false));
 *     iio_channel_enable(iio_device_find_channel(rx, "voltage1", false));
 *     struct iio_buffer *buf = iio_device_create_buffer(rx, 4096, false);
 *     iio_buffer_refill(buf);              // 4096 samples of I and Q
 *     for (char *p = iio_buffer_start(buf); p < (char *)iio_buffer_end(buf);
 *          p += iio_buffer_step(buf)) ...
 *
 * Backends: "ip:HOST" or "ip:HOST:PORT" (a name or an address; IPv4) and
 * "xml:FILE" (a context description only: its attributes cannot be read).
 * There are no local devices, and no "usb:" or "serial:" backends yet.
 * Calls that return int or ssize_t give a negative error number (-ENOENT)
 * on failure; calls that return a pointer give 0 and set errno.
 */
#ifndef RITTEROS_IIO_H
#define RITTEROS_IIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

struct iio_context;
struct iio_device;
struct iio_channel;
struct iio_buffer;
struct iio_context_info;
struct iio_scan_context;
struct iio_scan_block;

/* What a channel measures or makes, from its ID ("voltage0", "altvoltage1",
 * "temp0"...). */
enum iio_chan_type {
    IIO_VOLTAGE,
    IIO_CURRENT,
    IIO_POWER,
    IIO_ACCEL,
    IIO_ANGL_VEL,
    IIO_MAGN,
    IIO_LIGHT,
    IIO_INTENSITY,
    IIO_PROXIMITY,
    IIO_TEMP,
    IIO_INCLI,
    IIO_ROT,
    IIO_ANGL,
    IIO_TIMESTAMP,
    IIO_CAPACITANCE,
    IIO_ALTVOLTAGE,
    IIO_CCT,
    IIO_PRESSURE,
    IIO_HUMIDITYRELATIVE,
    IIO_ACTIVITY,
    IIO_STEPS,
    IIO_ENERGY,
    IIO_DISTANCE,
    IIO_VELOCITY,
    IIO_CONCENTRATION,
    IIO_RESISTANCE,
    IIO_PH,
    IIO_UVINDEX,
    IIO_ELECTRICALCONDUCTIVITY,
    IIO_COUNT,
    IIO_INDEX,
    IIO_GRAVITY,
    IIO_POSITIONRELATIVE,
    IIO_PHASE,
    IIO_MASSCONCENTRATION,
    IIO_CHAN_TYPE_UNKNOWN = 0x7FFFFFFF
};

/* The same for the channels of a hardware monitor (a device "hwmonN"). */
enum hwmon_chan_type {
    HWMON_VOLTAGE,
    HWMON_FAN,
    HWMON_PWM,
    HWMON_TEMP,
    HWMON_CURRENT,
    HWMON_POWER,
    HWMON_ENERGY,
    HWMON_HUMIDITY,
    HWMON_INTRUSION,
    HWMON_CHAN_TYPE_UNKNOWN = IIO_CHAN_TYPE_UNKNOWN
};

/* The part of the quantity a channel has, from its ID after a '_'
 * ("accel_x", "voltage0_i"...). */
enum iio_modifier {
    IIO_NO_MOD,
    IIO_MOD_X,
    IIO_MOD_Y,
    IIO_MOD_Z,
    IIO_MOD_X_AND_Y,
    IIO_MOD_X_AND_Z,
    IIO_MOD_Y_AND_Z,
    IIO_MOD_X_AND_Y_AND_Z,
    IIO_MOD_X_OR_Y,
    IIO_MOD_X_OR_Z,
    IIO_MOD_Y_OR_Z,
    IIO_MOD_X_OR_Y_OR_Z,
    IIO_MOD_LIGHT_BOTH,
    IIO_MOD_LIGHT_IR,
    IIO_MOD_ROOT_SUM_SQUARED_X_Y,
    IIO_MOD_SUM_SQUARED_X_Y_Z,
    IIO_MOD_LIGHT_CLEAR,
    IIO_MOD_LIGHT_RED,
    IIO_MOD_LIGHT_GREEN,
    IIO_MOD_LIGHT_BLUE,
    IIO_MOD_QUATERNION,
    IIO_MOD_TEMP_AMBIENT,
    IIO_MOD_TEMP_OBJECT,
    IIO_MOD_NORTH_MAGN,
    IIO_MOD_NORTH_TRUE,
    IIO_MOD_NORTH_MAGN_TILT_COMP,
    IIO_MOD_NORTH_TRUE_TILT_COMP,
    IIO_MOD_RUNNING,
    IIO_MOD_JOGGING,
    IIO_MOD_WALKING,
    IIO_MOD_STILL,
    IIO_MOD_ROOT_SUM_SQUARED_X_Y_Z,
    IIO_MOD_I,
    IIO_MOD_Q,
    IIO_MOD_CO2,
    IIO_MOD_VOC,
    IIO_MOD_LIGHT_UV,
    IIO_MOD_LIGHT_DUV,
    IIO_MOD_PM1,
    IIO_MOD_PM2P5,
    IIO_MOD_PM4,
    IIO_MOD_PM10,
    IIO_MOD_ETHANOL,
    IIO_MOD_H2,
    IIO_MOD_O2,
    IIO_MOD_LINEAR_X,
    IIO_MOD_LINEAR_Y,
    IIO_MOD_LINEAR_Z,
    IIO_MOD_PITCH,
    IIO_MOD_YAW,
    IIO_MOD_ROLL
};

/* How a sample of a channel lies in a buffer (its scan element's format,
 * "le:S12/16>>4": little-endian, signed, 12 bits in 16, shifted by 4). */
struct iio_data_format {
    unsigned int length;            /* bits a sample takes in the buffer */
    unsigned int bits;              /* bits that carry the value */
    unsigned int shift;             /* the value's place: shift right by this */
    bool is_signed;
    bool is_fully_defined;          /* the value fills every bit (or 'S'/'U') */
    bool is_be;                     /* big-endian */
    bool with_scale;
    double scale;                   /* value * scale = the quantity */
    unsigned int repeat;            /* samples per scan (X in "S16/16X2") */
};

/* --- Finding contexts --- */

/* A scan for contexts: there is no network discovery (DNS-SD) yet, so it
 * finds none (0 entries); give the address in the URI instead. */
struct iio_scan_context *iio_create_scan_context(const char *backend, unsigned int flags);
void iio_scan_context_destroy(struct iio_scan_context *ctx);
ssize_t iio_scan_context_get_info_list(struct iio_scan_context *ctx,
                                       struct iio_context_info ***info);
void iio_context_info_list_free(struct iio_context_info **info);
const char *iio_context_info_get_description(const struct iio_context_info *info);
const char *iio_context_info_get_uri(const struct iio_context_info *info);
struct iio_scan_block *iio_create_scan_block(const char *backend, unsigned int flags);
void iio_scan_block_destroy(struct iio_scan_block *blk);
ssize_t iio_scan_block_scan(struct iio_scan_block *blk);
struct iio_context_info *iio_scan_block_get_info(struct iio_scan_block *blk, unsigned int index);

/* --- The library --- */

/* The interface's version (0.26) and "ritter" as its tag. */
void iio_library_get_version(unsigned int *major, unsigned int *minor, char git_tag[8]);
/* The text of error number err (positive or negative) into dst. */
void iio_strerror(int err, char *dst, size_t len);
/* The backends: "ip" and "xml". */
bool iio_has_backend(const char *backend);
unsigned int iio_get_backends_count(void);
const char *iio_get_backend(unsigned int index);

/* --- Contexts: one machine's devices --- */

/* No local devices on RitterOS: these two fail (ENOSYS). */
struct iio_context *iio_create_default_context(void);
struct iio_context *iio_create_local_context(void);
/* A context from a description (iio_context_get_xml's text). */
struct iio_context *iio_create_xml_context(const char *xml_file);
struct iio_context *iio_create_xml_context_mem(const char *xml, size_t len);
/* The context of the iiod at host (a name or an address, ":PORT" allowed). */
struct iio_context *iio_create_network_context(const char *host);
/* "ip:HOST[:PORT]" or "xml:FILE". */
struct iio_context *iio_create_context_from_uri(const char *uri);
/* A second, independent context of the same machine (a connection of its
 * own). */
struct iio_context *iio_context_clone(const struct iio_context *ctx);
void iio_context_destroy(struct iio_context *ctx);
/* The server's version (iiod's), or the library's for an XML context. */
int iio_context_get_version(const struct iio_context *ctx, unsigned int *major,
                            unsigned int *minor, char git_tag[8]);
const char *iio_context_get_xml(const struct iio_context *ctx);
const char *iio_context_get_name(const struct iio_context *ctx);         /* "network", "xml" */
const char *iio_context_get_description(const struct iio_context *ctx);
/* Context attributes: the description's (hw_model, fw_version, ...) and
 * the network's ("ip,ip-addr", "uri"). */
unsigned int iio_context_get_attrs_count(const struct iio_context *ctx);
int iio_context_get_attr(const struct iio_context *ctx, unsigned int index, const char **name,
                         const char **value);
const char *iio_context_get_attr_value(const struct iio_context *ctx, const char *name);
unsigned int iio_context_get_devices_count(const struct iio_context *ctx);
struct iio_device *iio_context_get_device(const struct iio_context *ctx, unsigned int index);
/* By ID ("iio:device3"), label or name ("cf-ad9361-lpc"). */
struct iio_device *iio_context_find_device(const struct iio_context *ctx, const char *name);
/* How long a call waits for the server (default 5000 ms; 0: for ever). */
int iio_context_set_timeout(struct iio_context *ctx, unsigned int timeout_ms);

/* --- Devices --- */

const struct iio_context *iio_device_get_context(const struct iio_device *dev);
const char *iio_device_get_id(const struct iio_device *dev);
const char *iio_device_get_name(const struct iio_device *dev);     /* 0 when it has none */
const char *iio_device_get_label(const struct iio_device *dev);    /* 0 when it has none */
unsigned int iio_device_get_channels_count(const struct iio_device *dev);
struct iio_channel *iio_device_get_channel(const struct iio_device *dev, unsigned int index);
/* By ID or name, input or output ("altvoltage0" and "RX_LO" alike). */
struct iio_channel *iio_device_find_channel(const struct iio_device *dev, const char *name,
                                            bool output);
unsigned int iio_device_get_attrs_count(const struct iio_device *dev);
const char *iio_device_get_attr(const struct iio_device *dev, unsigned int index);
const char *iio_device_find_attr(const struct iio_device *dev, const char *name);
unsigned int iio_device_get_buffer_attrs_count(const struct iio_device *dev);
const char *iio_device_get_buffer_attr(const struct iio_device *dev, unsigned int index);
const char *iio_device_find_buffer_attr(const struct iio_device *dev, const char *name);
unsigned int iio_device_get_debug_attrs_count(const struct iio_device *dev);
const char *iio_device_get_debug_attr(const struct iio_device *dev, unsigned int index);
const char *iio_device_find_debug_attr(const struct iio_device *dev, const char *name);
/* The channel and attribute a sysfs file name stands for
 * ("in_voltage0_hardwaregain": voltage0's "hardwaregain"; a device
 * attribute's channel is 0). */
int iio_device_identify_filename(const struct iio_device *dev, const char *filename,
                                 struct iio_channel **chn, const char **attr);

/* An attribute's value as text (no line break at the end), the bytes read
 * (dst always ends in a 0); its value as a number, true/false, or written
 * (the bytes written). The _all calls go through every attribute: read_all
 * hands cb each value; write_all asks cb for each one (cb returns the
 * length, 0 to leave it, or a negative error). */
ssize_t iio_device_attr_read(const struct iio_device *dev, const char *attr, char *dst,
                             size_t len);
int iio_device_attr_read_all(struct iio_device *dev,
                             int (*cb)(struct iio_device *dev, const char *attr,
                                       const char *value, size_t len, void *d),
                             void *data);
int iio_device_attr_read_bool(const struct iio_device *dev, const char *attr, bool *val);
int iio_device_attr_read_longlong(const struct iio_device *dev, const char *attr, long long *val);
int iio_device_attr_read_double(const struct iio_device *dev, const char *attr, double *val);
ssize_t iio_device_attr_write(const struct iio_device *dev, const char *attr, const char *src);
ssize_t iio_device_attr_write_raw(const struct iio_device *dev, const char *attr, const void *src,
                                  size_t len);
int iio_device_attr_write_all(struct iio_device *dev,
                              ssize_t (*cb)(struct iio_device *dev, const char *attr, void *buf,
                                            size_t len, void *d),
                              void *data);
int iio_device_attr_write_bool(const struct iio_device *dev, const char *attr, bool val);
int iio_device_attr_write_longlong(const struct iio_device *dev, const char *attr, long long val);
int iio_device_attr_write_double(const struct iio_device *dev, const char *attr, double val);

/* The same for buffer attributes ... */
ssize_t iio_device_buffer_attr_read(const struct iio_device *dev, const char *attr, char *dst,
                                    size_t len);
int iio_device_buffer_attr_read_all(struct iio_device *dev,
                                    int (*cb)(struct iio_device *dev, const char *attr,
                                              const char *value, size_t len, void *d),
                                    void *data);
int iio_device_buffer_attr_read_bool(const struct iio_device *dev, const char *attr, bool *val);
int iio_device_buffer_attr_read_longlong(const struct iio_device *dev, const char *attr,
                                         long long *val);
int iio_device_buffer_attr_read_double(const struct iio_device *dev, const char *attr,
                                       double *val);
ssize_t iio_device_buffer_attr_write(const struct iio_device *dev, const char *attr,
                                     const char *src);
ssize_t iio_device_buffer_attr_write_raw(const struct iio_device *dev, const char *attr,
                                         const void *src, size_t len);
int iio_device_buffer_attr_write_all(struct iio_device *dev,
                                     ssize_t (*cb)(struct iio_device *dev, const char *attr,
                                                   void *buf, size_t len, void *d),
                                     void *data);
int iio_device_buffer_attr_write_bool(const struct iio_device *dev, const char *attr, bool val);
int iio_device_buffer_attr_write_longlong(const struct iio_device *dev, const char *attr,
                                          long long val);
int iio_device_buffer_attr_write_double(const struct iio_device *dev, const char *attr,
                                        double val);

/* ... and for debug attributes (the driver's own, in debugfs). */
ssize_t iio_device_debug_attr_read(const struct iio_device *dev, const char *attr, char *dst,
                                   size_t len);
int iio_device_debug_attr_read_all(struct iio_device *dev,
                                   int (*cb)(struct iio_device *dev, const char *attr,
                                             const char *value, size_t len, void *d),
                                   void *data);
ssize_t iio_device_debug_attr_write(const struct iio_device *dev, const char *attr,
                                    const char *src);
ssize_t iio_device_debug_attr_write_raw(const struct iio_device *dev, const char *attr,
                                        const void *src, size_t len);
int iio_device_debug_attr_write_all(struct iio_device *dev,
                                    ssize_t (*cb)(struct iio_device *dev, const char *attr,
                                                  void *buf, size_t len, void *d),
                                    void *data);
int iio_device_debug_attr_read_bool(const struct iio_device *dev, const char *attr, bool *val);
int iio_device_debug_attr_read_longlong(const struct iio_device *dev, const char *attr,
                                        long long *val);
int iio_device_debug_attr_read_double(const struct iio_device *dev, const char *attr,
                                      double *val);
int iio_device_debug_attr_write_bool(const struct iio_device *dev, const char *attr, bool val);
int iio_device_debug_attr_write_longlong(const struct iio_device *dev, const char *attr,
                                         long long val);
int iio_device_debug_attr_write_double(const struct iio_device *dev, const char *attr,
                                       double val);
/* A register of the chip, through the debug attribute direct_reg_access. */
int iio_device_reg_write(struct iio_device *dev, uint32_t address, uint32_t value);
int iio_device_reg_read(struct iio_device *dev, uint32_t address, uint32_t *value);

/* Data of the program's own, kept with the device. */
void iio_device_set_data(struct iio_device *dev, void *data);
void *iio_device_get_data(const struct iio_device *dev);
/* Triggers: the device that starts a device's conversions. */
int iio_device_get_trigger(const struct iio_device *dev, const struct iio_device **trigger);
int iio_device_set_trigger(const struct iio_device *dev, const struct iio_device *trigger);
bool iio_device_is_trigger(const struct iio_device *dev);
bool iio_device_is_hwmon(const struct iio_device *dev);
/* How many blocks the server's kernel keeps for the device's buffer. */
int iio_device_set_kernel_buffers_count(const struct iio_device *dev, unsigned int nb_buffers);

/* --- Channels --- */

const struct iio_device *iio_channel_get_device(const struct iio_channel *chn);
const char *iio_channel_get_id(const struct iio_channel *chn);
const char *iio_channel_get_name(const struct iio_channel *chn);   /* 0 when it has none */
bool iio_channel_is_output(const struct iio_channel *chn);
/* A scan element: the channel can go into a buffer. */
bool iio_channel_is_scan_element(const struct iio_channel *chn);
enum iio_chan_type iio_channel_get_type(const struct iio_channel *chn);
enum iio_modifier iio_channel_get_modifier(const struct iio_channel *chn);
unsigned int iio_channel_get_attrs_count(const struct iio_channel *chn);
const char *iio_channel_get_attr(const struct iio_channel *chn, unsigned int index);
const char *iio_channel_find_attr(const struct iio_channel *chn, const char *name);
/* The attribute's sysfs file name on the server ("out_altvoltage0_RX_LO_frequency"). */
const char *iio_channel_attr_get_filename(const struct iio_channel *chn, const char *attr);
ssize_t iio_channel_attr_read(const struct iio_channel *chn, const char *attr, char *dst,
                              size_t len);
int iio_channel_attr_read_all(struct iio_channel *chn,
                              int (*cb)(struct iio_channel *chn, const char *attr,
                                        const char *val, size_t len, void *d),
                              void *data);
int iio_channel_attr_read_bool(const struct iio_channel *chn, const char *attr, bool *val);
int iio_channel_attr_read_longlong(const struct iio_channel *chn, const char *attr,
                                   long long *val);
int iio_channel_attr_read_double(const struct iio_channel *chn, const char *attr, double *val);
ssize_t iio_channel_attr_write(const struct iio_channel *chn, const char *attr, const char *src);
ssize_t iio_channel_attr_write_raw(const struct iio_channel *chn, const char *attr,
                                   const void *src, size_t len);
int iio_channel_attr_write_all(struct iio_channel *chn,
                               ssize_t (*cb)(struct iio_channel *chn, const char *attr,
                                             void *buf, size_t len, void *d),
                               void *data);
int iio_channel_attr_write_bool(const struct iio_channel *chn, const char *attr, bool val);
int iio_channel_attr_write_longlong(const struct iio_channel *chn, const char *attr,
                                    long long val);
int iio_channel_attr_write_double(const struct iio_channel *chn, const char *attr, double val);
void iio_channel_set_data(struct iio_channel *chn, void *data);
void *iio_channel_get_data(const struct iio_channel *chn);

/* Which channels a buffer made afterwards carries (scan elements only). */
void iio_channel_enable(struct iio_channel *chn);
void iio_channel_disable(struct iio_channel *chn);
bool iio_channel_is_enabled(const struct iio_channel *chn);
/* The channel's place in a scan (-1: not a scan element) and its format. */
long iio_channel_get_index(const struct iio_channel *chn);
const struct iio_data_format *iio_channel_get_data_format(const struct iio_channel *chn);
/* One sample from the buffer's format to the CPU's (sign-extended, shifted;
 * src and dst length/8 bytes), and back. */
void iio_channel_convert(const struct iio_channel *chn, void *dst, const void *src);
void iio_channel_convert_inverse(const struct iio_channel *chn, void *dst, const void *src);

/* --- Buffers: streams of samples --- */

/* The bytes of one scan (every enabled channel's sample, each aligned to
 * its own size); negative: an error. */
ssize_t iio_device_get_sample_size(const struct iio_device *dev);
/* A buffer of samples_count scans of the enabled channels (a connection of
 * its own to the server). An input device's buffer is filled by refill; an
 * output device's is sent by push; cyclic: sent once, then repeated by the
 * hardware until the buffer is destroyed. */
struct iio_buffer *iio_device_create_buffer(const struct iio_device *dev, size_t samples_count,
                                            bool cyclic);
void iio_buffer_destroy(struct iio_buffer *buf);
const struct iio_device *iio_buffer_get_device(const struct iio_buffer *buf);
/* Fill the buffer: the bytes received, or a negative error. */
ssize_t iio_buffer_refill(struct iio_buffer *buf);
/* Send the buffer (or its first samples_count scans): the bytes sent. */
ssize_t iio_buffer_push(struct iio_buffer *buf);
ssize_t iio_buffer_push_partial(struct iio_buffer *buf, size_t samples_count);
/* Make a refill or push that waits (in another thread) return -EBADF;
 * the buffer can then only be destroyed. */
void iio_buffer_cancel(struct iio_buffer *buf);
/* Calls always wait: -ENOSYS for false; no descriptor to wait on (-ENOSYS). */
int iio_buffer_set_blocking_mode(struct iio_buffer *buf, bool blocking);
int iio_buffer_get_poll_fd(struct iio_buffer *buf);
/* The samples: start and end of the data, the first sample of a channel
 * and the step from one scan to the next. */
void *iio_buffer_start(const struct iio_buffer *buf);
void *iio_buffer_first(const struct iio_buffer *buf, const struct iio_channel *chn);
ptrdiff_t iio_buffer_step(const struct iio_buffer *buf);
void *iio_buffer_end(const struct iio_buffer *buf);
/* callback(channel, sample, bytes, data) for each sample of each enabled
 * channel in order; stops at a negative result. The sum of the results. */
ssize_t iio_buffer_foreach_sample(struct iio_buffer *buf,
                                  ssize_t (*callback)(const struct iio_channel *chn, void *src,
                                                      size_t bytes, void *d),
                                  void *data);
void iio_buffer_set_data(struct iio_buffer *buf, void *data);
void *iio_buffer_get_data(const struct iio_buffer *buf);
/* One channel's samples out of the buffer into dst, or from src into it
 * (raw: as stored; otherwise converted): the bytes copied. */
size_t iio_channel_read_raw(const struct iio_channel *chn, struct iio_buffer *buffer, void *dst,
                            size_t len);
size_t iio_channel_read(const struct iio_channel *chn, struct iio_buffer *buffer, void *dst,
                        size_t len);
size_t iio_channel_write_raw(const struct iio_channel *chn, struct iio_buffer *buffer,
                             const void *src, size_t len);
size_t iio_channel_write(const struct iio_channel *chn, struct iio_buffer *buffer,
                         const void *src, size_t len);

#endif
