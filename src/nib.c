#include "nib.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

#define MAX_DEPTH 64

/* ---- a small XML reader: elements, attributes, text and entities ---- */

typedef struct {
    char *key, *val;
} xattr;

typedef struct xnode {
    char *tag;
    xattr *attrs;
    int nattrs;
    struct xnode **kids;
    int nkids, kcap;
    char *text; /* character data, entities decoded */
    size_t tlen;
} xnode;

typedef struct {
    const char *p, *end;
    char *err;
    size_t errlen;
} reader;

static bool fail(char *err, size_t errlen, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

static bool fail(char *err, size_t errlen, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
    return false;
}

static void *xalloc(size_t n) {
    void *p = calloc(1, n ? n : 1);
    if (!p)
        fatal("out of memory");
    return p;
}

static void free_node(xnode *n) {
    if (!n)
        return;
    for (int i = 0; i < n->nattrs; i++) {
        free(n->attrs[i].key);
        free(n->attrs[i].val);
    }
    for (int i = 0; i < n->nkids; i++)
        free_node(n->kids[i]);
    free(n->attrs);
    free(n->kids);
    free(n->tag);
    free(n->text);
    free(n);
}

static void skip_ws(reader *r) {
    while (r->p < r->end && (*r->p == ' ' || *r->p == '\t' || *r->p == '\n' || *r->p == '\r'))
        r->p++;
}

static bool starts(reader *r, const char *s) {
    size_t n = strlen(s);
    return (size_t)(r->end - r->p) >= n && memcmp(r->p, s, n) == 0;
}

static bool skip_past(reader *r, const char *s) {
    size_t n = strlen(s);
    for (; r->p + n <= r->end; r->p++)
        if (memcmp(r->p, s, n) == 0) {
            r->p += n;
            return true;
        }
    return fail(r->err, r->errlen, "unterminated markup (no \"%s\")", s);
}

static bool name_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
           c == ':' || c == '.' || c == '-';
}

static char *read_name(reader *r) {
    const char *s = r->p;
    while (r->p < r->end && name_char(*r->p))
        r->p++;
    if (r->p == s) {
        fail(r->err, r->errlen, "expected a name at \"%.20s\"", s);
        return NULL;
    }
    return strndup(s, (size_t)(r->p - s));
}

static void put_utf8(char *out, size_t *n, uint32_t c) {
    if (c < 0x80) {
        out[(*n)++] = (char)c;
    } else if (c < 0x800) {
        out[(*n)++] = (char)(0xC0 | (c >> 6));
        out[(*n)++] = (char)(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out[(*n)++] = (char)(0xE0 | (c >> 12));
        out[(*n)++] = (char)(0x80 | ((c >> 6) & 0x3F));
        out[(*n)++] = (char)(0x80 | (c & 0x3F));
    } else {
        out[(*n)++] = (char)(0xF0 | (c >> 18));
        out[(*n)++] = (char)(0x80 | ((c >> 12) & 0x3F));
        out[(*n)++] = (char)(0x80 | ((c >> 6) & 0x3F));
        out[(*n)++] = (char)(0x80 | (c & 0x3F));
    }
}

/* Decodes s[0..len) with its entities into a new string. */
static char *decode(reader *r, const char *s, size_t len) {
    char *out = xalloc(len + 1); /* decoding never grows the text */
    size_t n = 0;
    for (size_t i = 0; i < len;) {
        if (s[i] != '&') {
            out[n++] = s[i++];
            continue;
        }
        const char *semi = memchr(s + i, ';', len - i);
        if (!semi || semi - (s + i) > 10) {
            fail(r->err, r->errlen, "a bad entity in \"%.20s\"", s + i);
            free(out);
            return NULL;
        }
        size_t elen = (size_t)(semi - (s + i)) + 1;
        static const struct {
            const char *name;
            char c;
        } named[] = {{"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}};
        bool done = false;
        for (size_t k = 0; k < sizeof named / sizeof named[0] && !done; k++)
            if (elen == strlen(named[k].name) && memcmp(s + i, named[k].name, elen) == 0) {
                out[n++] = named[k].c;
                done = true;
            }
        if (!done && elen > 3 && s[i + 1] == '#') {
            char *end;
            unsigned long c = s[i + 2] == 'x' ? strtoul(s + i + 3, &end, 16) : strtoul(s + i + 2, &end, 10);
            if (end == semi && c > 0 && c < 0x110000) {
                put_utf8(out, &n, (uint32_t)c);
                done = true;
            }
        }
        if (!done) {
            fail(r->err, r->errlen, "an unknown entity %.*s", (int)elen, s + i);
            free(out);
            return NULL;
        }
        i += elen;
    }
    out[n] = '\0';
    return out;
}

static void add_kid(xnode *n, xnode *k) {
    if (n->nkids == n->kcap) {
        n->kcap = n->kcap ? n->kcap * 2 : 8;
        n->kids = realloc(n->kids, (size_t)n->kcap * sizeof *n->kids);
        if (!n->kids)
            fatal("out of memory");
    }
    n->kids[n->nkids++] = k;
}

static void add_text(xnode *n, const char *s) {
    size_t len = strlen(s);
    n->text = realloc(n->text, n->tlen + len + 1);
    if (!n->text)
        fatal("out of memory");
    memcpy(n->text + n->tlen, s, len + 1);
    n->tlen += len;
}

static xnode *parse_element(reader *r, int depth) {
    if (depth > MAX_DEPTH) {
        fail(r->err, r->errlen, "elements are nested too deeply");
        return NULL;
    }
    if (!starts(r, "<")) {
        fail(r->err, r->errlen, "expected an element");
        return NULL;
    }
    r->p++;
    xnode *n = xalloc(sizeof *n);
    if (!(n->tag = read_name(r)))
        goto bad;
    for (;;) {
        skip_ws(r);
        if (starts(r, "/>")) {
            r->p += 2;
            return n;
        }
        if (starts(r, ">")) {
            r->p++;
            break;
        }
        char *key = read_name(r);
        if (!key)
            goto bad;
        skip_ws(r);
        if (!starts(r, "=")) {
            free(key);
            fail(r->err, r->errlen, "attribute %s has no value", n->tag);
            goto bad;
        }
        r->p++;
        skip_ws(r);
        char q = r->p < r->end ? *r->p : 0;
        const char *close = (q == '"' || q == '\'') ? memchr(r->p + 1, q, (size_t)(r->end - r->p - 1)) : NULL;
        if (!close) {
            free(key);
            fail(r->err, r->errlen, "attribute of <%s> isn't quoted", n->tag);
            goto bad;
        }
        char *val = decode(r, r->p + 1, (size_t)(close - r->p - 1));
        if (!val) {
            free(key);
            goto bad;
        }
        r->p = close + 1;
        n->attrs = realloc(n->attrs, (size_t)(n->nattrs + 1) * sizeof *n->attrs);
        if (!n->attrs)
            fatal("out of memory");
        n->attrs[n->nattrs++] = (xattr){key, val};
    }
    for (;;) {
        if (r->p >= r->end) {
            fail(r->err, r->errlen, "<%s> is never closed", n->tag);
            goto bad;
        }
        if (starts(r, "</")) {
            r->p += 2;
            char *name = read_name(r);
            bool same = name && strcmp(name, n->tag) == 0;
            free(name);
            skip_ws(r);
            if (!same || !starts(r, ">")) {
                fail(r->err, r->errlen, "<%s> is closed by another tag", n->tag);
                goto bad;
            }
            r->p++;
            return n;
        }
        if (starts(r, "<!--")) {
            if (!skip_past(r, "-->"))
                goto bad;
            continue;
        }
        if (starts(r, "<")) {
            xnode *k = parse_element(r, depth + 1);
            if (!k)
                goto bad;
            add_kid(n, k);
            continue;
        }
        const char *lt = memchr(r->p, '<', (size_t)(r->end - r->p));
        if (!lt)
            lt = r->end;
        char *t = decode(r, r->p, (size_t)(lt - r->p));
        if (!t)
            goto bad;
        add_text(n, t);
        free(t);
        r->p = lt;
    }
bad:
    free_node(n);
    return NULL;
}

static xnode *parse_document(const char *xml, size_t len, char *err, size_t errlen) {
    reader r = {xml, xml + len, err, errlen};
    skip_ws(&r);
    while (starts(&r, "<?") || starts(&r, "<!--")) {
        if (!skip_past(&r, starts(&r, "<?") ? "?>" : "-->"))
            return NULL;
        skip_ws(&r);
    }
    xnode *root = parse_element(&r, 0);
    if (!root)
        return NULL;
    skip_ws(&r);
    if (r.p != r.end) {
        free_node(root);
        fail(err, errlen, "text after the document's element");
        return NULL;
    }
    return root;
}

/* ---- the nib's objects ---- */

static const char *attr(const xnode *n, const char *key) {
    for (int i = 0; i < n->nattrs; i++)
        if (strcmp(n->attrs[i].key, key) == 0)
            return n->attrs[i].val;
    return NULL;
}

/* The child called name (its name attribute), or NULL. */
static const xnode *field(const xnode *n, const char *name) {
    for (int i = 0; i < n->nkids; i++) {
        const char *a = attr(n->kids[i], "name");
        if (a && strcmp(a, name) == 0)
            return n->kids[i];
    }
    return NULL;
}

static const char *field_text(const xnode *n, const char *name) {
    const xnode *f = field(n, name);
    return f ? (f->text ? f->text : "") : NULL;
}

typedef struct {
    const xnode **nodes;
    const char **ids;
    int n, cap;
} id_index;

static void index_ids(id_index *ix, const xnode *n) {
    const char *id = attr(n, "id");
    if (id && strcmp(n->tag, "object") == 0) {
        if (ix->n == ix->cap) {
            ix->cap = ix->cap ? ix->cap * 2 : 64;
            ix->nodes = realloc(ix->nodes, (size_t)ix->cap * sizeof *ix->nodes);
            ix->ids = realloc(ix->ids, (size_t)ix->cap * sizeof *ix->ids);
            if (!ix->nodes || !ix->ids)
                fatal("out of memory");
        }
        ix->nodes[ix->n] = n;
        ix->ids[ix->n++] = id;
    }
    for (int i = 0; i < n->nkids; i++)
        index_ids(ix, n->kids[i]);
}

/* An object, following a <reference idRef=...>. NULL if it names nothing. */
static const xnode *resolve(const id_index *ix, const xnode *n) {
    if (!n || strcmp(n->tag, "reference") != 0)
        return n;
    const char *ref = attr(n, "idRef");
    for (int i = 0; ref && i < ix->n; i++)
        if (strcmp(ix->ids[i], ref) == 0)
            return ix->nodes[i];
    return NULL;
}

static bool read_rect(const char *s, nib_rect *out) {
    return s && sscanf(s, "%d %d %d %d", &out->top, &out->left, &out->bottom, &out->right) == 4;
}

static bool read_ostype(const char *s, uint32_t *out) {
    if (!s) {
        *out = 0;
        return true;
    }
    if (strlen(s) != 4)
        return false;
    *out = FOURCC(s[0], s[1], s[2], s[3]);
    return true;
}

static bool read_control(const xnode *o, nib_control *c, char *err, size_t errlen) {
    const char *cls = attr(o, "class");
    const char *id = attr(o, "id");
    memset(c, 0, sizeof *c);
    if (!cls)
        return fail(err, errlen, "control %s has no class", id ? id : "?");
    if (strcmp(cls, "IBCarbonButton") == 0)
        c->kind = NIB_BUTTON;
    else if (strcmp(cls, "IBCarbonStaticText") == 0)
        c->kind = NIB_STATIC_TEXT;
    else if (strcmp(cls, "IBCarbonEditText") == 0)
        c->kind = NIB_EDIT_TEXT;
    else if (strcmp(cls, "IBCarbonImageView") == 0)
        c->kind = NIB_IMAGE_VIEW;
    else if (strcmp(cls, "IBCarbonIcon") == 0)
        c->kind = NIB_ICON;
    else
        return fail(err, errlen, "control %s is an %s, which isn't supported", id ? id : "?", cls);
    if (!read_rect(field_text(o, "bounds"), &c->bounds))
        return fail(err, errlen, "control %s has no bounds", id ? id : "?");
    const char *title = field_text(o, "title");
    snprintf(c->title, sizeof c->title, "%s", title ? title : "");
    if (!read_ostype(field_text(o, "command"), &c->command) ||
        !read_ostype(field_text(o, "controlSignature"), &c->signature))
        return fail(err, errlen, "control %s has a bad four-character code", id ? id : "?");
    const char *v = field_text(o, "buttonType");
    c->button_type = v ? atoi(v) : 0;
    v = field_text(o, "controlID");
    c->id = v ? (int32_t)atol(v) : 0;
    return true;
}

static bool read_window(const xnode *root, const id_index *ix, const char *name, nib_window *w,
                        char *err, size_t errlen) {
    const xnode *table = field(root, "nameTable");
    if (!table)
        return fail(err, errlen, "the nib has no nameTable");
    const xnode *win = NULL;
    for (int i = 0; i + 1 < table->nkids && !win; i += 2) {
        const xnode *key = table->kids[i];
        if (strcmp(key->tag, "string") == 0 && key->text && strcmp(key->text, name) == 0) {
            win = resolve(ix, table->kids[i + 1]);
            if (!win)
                return fail(err, errlen, "the nib's %s names no object", name);
        }
    }
    if (!win)
        return fail(err, errlen, "the nib has no window called %s", name);
    const char *cls = attr(win, "class");
    if (!cls || strcmp(cls, "IBCarbonWindow") != 0)
        return fail(err, errlen, "the nib's %s is a %s, not a window", name, cls ? cls : "?");

    memset(w, 0, sizeof *w);
    snprintf(w->name, sizeof w->name, "%s", name);
    const char *title = field_text(win, "title");
    snprintf(w->title, sizeof w->title, "%s", title ? title : "");
    if (!read_rect(field_text(win, "windowRect"), &w->rect))
        return fail(err, errlen, "window %s has no windowRect", name);
    const char *v = field_text(win, "carbonWindowClass");
    w->window_class = v ? atoi(v) : 0;

    const xnode *rc = resolve(ix, field(win, "rootControl"));
    if (!rc)
        return fail(err, errlen, "window %s has no root control", name);
    const xnode *views = field(rc, "subviews");
    for (int i = 0; views && i < views->nkids; i++) {
        const xnode *o = resolve(ix, views->kids[i]);
        if (!o)
            return fail(err, errlen, "window %s refers to an object that isn't there", name);
        if (w->ncontrols == NIB_MAX_CONTROLS)
            return fail(err, errlen, "window %s has more than %d controls", name, NIB_MAX_CONTROLS);
        if (!read_control(o, &w->controls[w->ncontrols], err, errlen))
            return false;
        w->ncontrols++;
    }
    return true;
}

bool nib_read_window(const char *xml, size_t len, const char *name, nib_window *out, char *err,
                     size_t errlen) {
    xnode *root = parse_document(xml, len, err, errlen);
    if (!root)
        return false;
    id_index ix = {0};
    index_ids(&ix, root);
    bool ok = read_window(root, &ix, name, out, err, errlen);
    free(ix.nodes);
    free(ix.ids);
    free_node(root);
    return ok;
}
