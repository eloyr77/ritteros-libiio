/*
 * A context from its XML description: what iiod's PRINT sends
 * and iio_context_get_xml returns. A small reader of just what that text
 * uses: the declaration and the DOCTYPE (skipped, with its internal
 * subset), comments, elements with quoted attributes, the five named
 * entities and numeric ones. The elements: context (description,
 * version-*), context-attribute, device (id, name, label), and in a device
 * channel (id, name, type) with scan-element (index, format, scale) and
 * attribute (name, filename), attribute, buffer-attribute, debug-attribute.
 * Unknown elements and attributes are passed over, as libiio does.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "iiopriv.h"

#define MAX_ATTRS 8
#define MAX_DEPTH 8

struct reader {
    const char *p, *end;
};

/* One element's start tag: its name and attributes (decoded copies). */
struct tag {
    char name[32];
    char *keys[MAX_ATTRS], *values[MAX_ATTRS];
    int count;
    bool empty;                     /* <x ... /> */
    bool closing;                   /* </x> */
};

static void tag_free(struct tag *t)
{
    for (int i = 0; i < t->count; i++)
        free(t->keys[i]), free(t->values[i]);
    t->count = 0;
}

static const char *tag_get(const struct tag *t, const char *key)
{
    for (int i = 0; i < t->count; i++)
        if (!strcmp(t->keys[i], key))
            return t->values[i];
    return 0;
}

static bool starts(const struct reader *r, const char *s)
{
    size_t n = strlen(s);
    return (size_t)(r->end - r->p) >= n && !memcmp(r->p, s, n);
}

static bool skip_past(struct reader *r, const char *s)
{
    size_t n = strlen(s);
    for (; (size_t)(r->end - r->p) >= n; r->p++)
        if (!memcmp(r->p, s, n)) {
            r->p += n;
            return true;
        }
    return false;
}

static bool space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static void skip_space(struct reader *r)
{
    while (r->p < r->end && space(*r->p))
        r->p++;
}

/* <!DOCTYPE ...>, with an internal subset in [ ] (quoted text in it may
 * hold '>' and ']'). */
static bool skip_doctype(struct reader *r)
{
    int brackets = 0;
    char quote = 0;
    for (; r->p < r->end; r->p++) {
        char c = *r->p;
        if (quote) {
            if (c == quote)
                quote = 0;
        } else if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == '[') {
            brackets++;
        } else if (c == ']') {
            brackets--;
        } else if (c == '>' && brackets <= 0) {
            r->p++;
            return true;
        }
    }
    return false;
}

static void put_utf8(char **o, unsigned long c)
{
    if (c < 0x80) {
        *(*o)++ = (char)c;
    } else if (c < 0x800) {
        *(*o)++ = (char)(0xC0 | c >> 6);
        *(*o)++ = (char)(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        *(*o)++ = (char)(0xE0 | c >> 12);
        *(*o)++ = (char)(0x80 | (c >> 6 & 0x3F));
        *(*o)++ = (char)(0x80 | (c & 0x3F));
    } else {
        *(*o)++ = (char)(0xF0 | c >> 18);
        *(*o)++ = (char)(0x80 | (c >> 12 & 0x3F));
        *(*o)++ = (char)(0x80 | (c >> 6 & 0x3F));
        *(*o)++ = (char)(0x80 | (c & 0x3F));
    }
}

/* An attribute's value from s (n bytes), entities replaced; 0 when one is
 * not known or out of memory. The text never grows (an entity is longer
 * than the at most four bytes it stands for). */
static char *decode(const char *s, size_t n)
{
    char *out = malloc(n + 1), *o = out;
    if (!out)
        return 0;
    for (size_t i = 0; i < n;) {
        if (s[i] != '&') {
            *o++ = s[i++];
            continue;
        }
        const char *semi = memchr(s + i, ';', n - i);
        if (!semi)
            goto bad;
        size_t len = (size_t)(semi - (s + i)) + 1;
        static const struct { const char *name; char c; } named[] = {
            { "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' }, { "&quot;", '"' }, { "&apos;", '\'' },
        };
        bool found = false;
        for (unsigned k = 0; k < sizeof(named) / sizeof(named[0]); k++)
            if (len == strlen(named[k].name) && !memcmp(s + i, named[k].name, len)) {
                *o++ = named[k].c;
                found = true;
            }
        if (!found) {
            if (len < 4 || s[i + 1] != '#')
                goto bad;
            unsigned long c = 0;
            bool hex = s[i + 2] == 'x' || s[i + 2] == 'X';
            size_t j = i + (hex ? 3 : 2);
            if (j >= i + len - 1)
                goto bad;
            for (; j < i + len - 1; j++) {
                char d = s[j];
                int v = d >= '0' && d <= '9' ? d - '0'
                      : hex && d >= 'a' && d <= 'f' ? d - 'a' + 10
                      : hex && d >= 'A' && d <= 'F' ? d - 'A' + 10 : -1;
                if (v < 0 || v >= (hex ? 16 : 10) || c > 0x10FFFF)
                    goto bad;
                c = c * (hex ? 16 : 10) + (unsigned long)v;
            }
            if (c == 0 || c > 0x10FFFF || len < 4)
                goto bad;
            put_utf8(&o, c);
        }
        i += len;
    }
    *o = 0;
    return out;
bad:
    free(out);
    return 0;
}

/* The next tag (text between tags is skipped). 1: a tag in *t, 0: the
 * end of the text, -EINVAL: not well formed. */
static int next_tag(struct reader *r, struct tag *t)
{
    memset(t, 0, sizeof(*t));
    for (;;) {
        while (r->p < r->end && *r->p != '<')
            r->p++;
        if (r->p >= r->end)
            return 0;
        if (starts(r, "<?")) {
            if (!skip_past(r, "?>"))
                return -EINVAL;
        } else if (starts(r, "<!--")) {
            if (!skip_past(r, "-->"))
                return -EINVAL;
        } else if (starts(r, "<!")) {
            if (!skip_doctype(r))
                return -EINVAL;
        } else {
            break;
        }
    }
    r->p++;
    if (r->p < r->end && *r->p == '/') {
        t->closing = true;
        r->p++;
    }
    size_t n = 0;
    while (r->p < r->end && !space(*r->p) && *r->p != '>' && *r->p != '/') {
        if (n + 1 >= sizeof(t->name))
            return -EINVAL;
        t->name[n++] = *r->p++;
    }
    if (n == 0)
        return -EINVAL;
    for (;;) {
        skip_space(r);
        if (r->p >= r->end)
            return -EINVAL;
        if (*r->p == '>') {
            r->p++;
            return 1;
        }
        if (*r->p == '/') {
            if (r->end - r->p < 2 || r->p[1] != '>' || t->closing)
                return -EINVAL;
            r->p += 2;
            t->empty = true;
            return 1;
        }
        if (t->closing)
            return -EINVAL;
        const char *key = r->p;
        while (r->p < r->end && !space(*r->p) && *r->p != '=' && *r->p != '>' && *r->p != '/')
            r->p++;
        size_t key_len = (size_t)(r->p - key);
        skip_space(r);
        if (key_len == 0 || r->p >= r->end || *r->p != '=')
            return -EINVAL;
        r->p++;
        skip_space(r);
        if (r->p >= r->end || (*r->p != '"' && *r->p != '\''))
            return -EINVAL;
        char quote = *r->p++;
        const char *value = r->p;
        while (r->p < r->end && *r->p != quote)
            r->p++;
        if (r->p >= r->end)
            return -EINVAL;
        size_t value_len = (size_t)(r->p - value);
        r->p++;
        if (t->count == MAX_ATTRS)
            continue;                       /* (more than any element here has) */
        char *k = malloc(key_len + 1), *v = decode(value, value_len);
        if (!k || !v) {
            free(k);
            free(v);
            return -EINVAL;
        }
        memcpy(k, key, key_len);
        k[key_len] = 0;
        t->keys[t->count] = k;
        t->values[t->count++] = v;
    }
}

char *iio_strdup(const char *s)
{
    if (!s)
        return 0;
    size_t n = strlen(s) + 1;
    char *t = malloc(n);
    if (t)
        memcpy(t, s, n);
    return t;
}

static bool grow(void *array, unsigned count, size_t size)
{
    void **a = array;
    void *bigger = realloc(*a, (count + 1) * size);
    if (!bigger)
        return false;
    *a = bigger;
    return true;
}

int iio_context_add_attr(struct iio_context *ctx, const char *name, const char *value)
{
    for (unsigned i = 0; i < ctx->nattrs; i++)
        if (!strcmp(ctx->attr_names[i], name)) {
            char *v = iio_strdup(value);
            if (!v)
                return -ENOMEM;
            free(ctx->attr_values[i]);
            ctx->attr_values[i] = v;
            return 0;
        }
    if (!grow(&ctx->attr_names, ctx->nattrs, sizeof(char *)) ||
        !grow(&ctx->attr_values, ctx->nattrs, sizeof(char *)))
        return -ENOMEM;
    char *n = iio_strdup(name), *v = iio_strdup(value);
    if (!n || !v) {
        free(n);
        free(v);
        return -ENOMEM;
    }
    ctx->attr_names[ctx->nattrs] = n;
    ctx->attr_values[ctx->nattrs++] = v;
    return 0;
}

static int add_attr(struct iio_attr **list, unsigned *count, const struct tag *t)
{
    const char *name = tag_get(t, "name");
    if (!name)
        return -EINVAL;
    if (!grow(list, *count, sizeof(struct iio_attr)))
        return -ENOMEM;
    struct iio_attr *a = &(*list)[*count];
    a->name = iio_strdup(name);
    a->filename = iio_strdup(tag_get(t, "filename"));
    if (!a->name)
        return -ENOMEM;
    (*count)++;
    return 0;
}

/* "le:S12/16>>4" or "be:u8/8X2>>0". */
static int parse_format(struct iio_data_format *f, const char *s)
{
    char *end;
    if ((s[0] != 'l' && s[0] != 'b') || s[1] != 'e' || s[2] != ':')
        return -EINVAL;
    f->is_be = s[0] == 'b';
    char sign = s[3];
    if (sign != 's' && sign != 'S' && sign != 'u' && sign != 'U')
        return -EINVAL;
    f->is_signed = sign == 's' || sign == 'S';
    f->bits = (unsigned)strtoul(s + 4, &end, 10);
    if (end == s + 4 || *end != '/')
        return -EINVAL;
    s = end + 1;
    f->length = (unsigned)strtoul(s, &end, 10);
    if (end == s)
        return -EINVAL;
    f->repeat = 1;
    if (*end == 'X') {
        s = end + 1;
        f->repeat = (unsigned)strtoul(s, &end, 10);
        if (end == s || f->repeat == 0)
            return -EINVAL;
    }
    if (end[0] != '>' || end[1] != '>')
        return -EINVAL;
    s = end + 2;
    f->shift = (unsigned)strtoul(s, &end, 10);
    if (end == s || *end)
        return -EINVAL;
    if (f->length == 0 || f->length > 64 || f->length % 8 || f->bits > f->length ||
        f->shift >= f->length)
        return -EINVAL;
    f->is_fully_defined = sign == 'S' || sign == 'U' || f->bits == f->length;
    return 0;
}

static struct iio_device *new_device(struct iio_context *ctx, const struct tag *t)
{
    const char *id = tag_get(t, "id");
    if (!id || !grow(&ctx->devices, ctx->ndevices, sizeof(struct iio_device *)))
        return 0;
    struct iio_device *dev = calloc(1, sizeof(*dev));
    if (!dev)
        return 0;
    dev->ctx = ctx;
    dev->id = iio_strdup(id);
    dev->name = iio_strdup(tag_get(t, "name"));
    dev->label = iio_strdup(tag_get(t, "label"));
    ctx->devices[ctx->ndevices++] = dev;
    return dev->id ? dev : 0;
}

static struct iio_channel *new_channel(struct iio_device *dev, const struct tag *t)
{
    const char *id = tag_get(t, "id"), *type = tag_get(t, "type");
    if (!id || !type || (strcmp(type, "input") && strcmp(type, "output")) ||
        !grow(&dev->channels, dev->nchannels, sizeof(struct iio_channel *)))
        return 0;
    struct iio_channel *chn = calloc(1, sizeof(*chn));
    if (!chn)
        return 0;
    chn->dev = dev;
    chn->id = iio_strdup(id);
    chn->name = iio_strdup(tag_get(t, "name"));
    chn->output = !strcmp(type, "output");
    chn->index = -1;
    chn->format.repeat = 1;
    dev->channels[dev->nchannels++] = chn;
    return chn->id ? chn : 0;
}

static int scan_element(struct iio_channel *chn, const struct tag *t)
{
    const char *index = tag_get(t, "index"), *format = tag_get(t, "format");
    const char *scale = tag_get(t, "scale");
    char *end;
    if (!index || !format)
        return -EINVAL;
    chn->index = strtol(index, &end, 10);
    if (end == index || *end || chn->index < 0)
        return -EINVAL;
    if (parse_format(&chn->format, format) < 0)
        return -EINVAL;
    if (scale) {
        chn->format.scale = strtod(scale, &end);
        chn->format.with_scale = end != scale;
    }
    chn->scan_element = true;
    return 0;
}

/* Scan elements first, by index (equal indexes by shift), the others after
 * in their order; then each channel's number is its place (the server
 * orders them the same way, and buffer masks count by it). */
static void order_channels(struct iio_device *dev)
{
    for (unsigned i = 1; i < dev->nchannels; i++) {
        struct iio_channel *c = dev->channels[i];
        unsigned j = i;
        for (; j > 0; j--) {
            struct iio_channel *p = dev->channels[j - 1];
            bool before = c->index >= 0 &&
                          (p->index < 0 || c->index < p->index ||
                           (c->index == p->index && c->format.shift < p->format.shift));
            if (!before)
                break;
            dev->channels[j] = p;
        }
        dev->channels[j] = c;
    }
    for (unsigned i = 0; i < dev->nchannels; i++)
        dev->channels[i]->number = i;
}

int iio_xml_parse(struct iio_context *ctx, const char *xml, size_t len)
{
    struct reader r = { xml, xml + len };
    struct tag t;
    char stack[MAX_DEPTH][32];
    int depth = 0, ret;
    bool seen_context = false;
    struct iio_device *dev = 0;
    struct iio_channel *chn = 0;

    while ((ret = next_tag(&r, &t)) == 1) {
        if (t.closing) {
            if (depth == 0 || strcmp(stack[depth - 1], t.name)) {
                ret = -EINVAL;
                break;
            }
            depth--;
            if (!strcmp(t.name, "device"))
                dev = 0;
            else if (!strcmp(t.name, "channel"))
                chn = 0;
            continue;
        }
        const char *parent = depth ? stack[depth - 1] : "";
        ret = 0;
        if (!strcmp(t.name, "context") && depth == 0) {
            if (seen_context) {
                ret = -EINVAL;
            } else {
                const char *d = tag_get(&t, "description");
                const char *maj = tag_get(&t, "version-major"), *min = tag_get(&t, "version-minor");
                const char *git = tag_get(&t, "version-git");
                seen_context = true;
                if (d) {
                    free(ctx->description);
                    ctx->description = iio_strdup(d);
                }
                if (maj && min && git) {
                    ctx->major = (unsigned)strtoul(maj, 0, 10);
                    ctx->minor = (unsigned)strtoul(min, 0, 10);
                    strncpy(ctx->git_tag, git, sizeof(ctx->git_tag) - 1);
                }
            }
        } else if (!seen_context) {
            ret = -EINVAL;                  /* the root must be <context> */
        } else if (!strcmp(t.name, "context-attribute") && !strcmp(parent, "context")) {
            const char *name = tag_get(&t, "name"), *value = tag_get(&t, "value");
            ret = name && value ? iio_context_add_attr(ctx, name, value) : -EINVAL;
        } else if (!strcmp(t.name, "device") && !strcmp(parent, "context")) {
            dev = new_device(ctx, &t);
            ret = dev ? 0 : -EINVAL;
        } else if (!strcmp(t.name, "channel") && dev && !strcmp(parent, "device")) {
            chn = new_channel(dev, &t);
            ret = chn ? 0 : -EINVAL;
        } else if (!strcmp(t.name, "scan-element") && chn && !strcmp(parent, "channel")) {
            ret = scan_element(chn, &t);
        } else if (!strcmp(t.name, "attribute") && chn && !strcmp(parent, "channel")) {
            ret = add_attr(&chn->attrs, &chn->nattrs, &t);
        } else if (!strcmp(t.name, "attribute") && dev && !strcmp(parent, "device")) {
            ret = add_attr(&dev->attrs, &dev->nattrs, &t);
        } else if (!strcmp(t.name, "buffer-attribute") && dev && !strcmp(parent, "device")) {
            ret = add_attr(&dev->buffer_attrs, &dev->nbuffer_attrs, &t);
        } else if (!strcmp(t.name, "debug-attribute") && dev && !strcmp(parent, "device")) {
            ret = add_attr(&dev->debug_attrs, &dev->ndebug_attrs, &t);
        }
        if (ret == 0 && !t.empty) {
            if (depth == MAX_DEPTH)
                ret = -EINVAL;
            else
                strcpy(stack[depth++], t.name);
        }
        tag_free(&t);
        if (ret < 0)
            break;
    }
    tag_free(&t);
    if (ret == 0 && (!seen_context || depth != 0))
        ret = -EINVAL;
    if (ret < 0)
        return ret;
    for (unsigned i = 0; i < ctx->ndevices; i++) {
        struct iio_device *d = ctx->devices[i];
        order_channels(d);
        for (unsigned k = 0; k < d->nchannels; k++)
            iio_channel_finish(d->channels[k]);
        d->words = (d->nchannels + 31) / 32;
        if (d->words && !(d->mask = calloc(d->words, sizeof(uint32_t))))
            return -ENOMEM;
    }
    return 0;
}
