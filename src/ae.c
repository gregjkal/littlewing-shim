#include "ae.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "guest_mem.h"
#include "memmgr.h"
#include "misc.h"
#include "snapshot.h"
#include "trap.h"
#include "util.h"

#define MAX_DESCS 512

#define TYPE_LIST 0x6C697374u     /* 'list' */
#define TYPE_RECORD 0x7265636Fu   /* 'reco' */
#define TYPE_WILD 0x2A2A2A2Au     /* '****' */
#define TYPE_OBJECT 0x6F626A20u   /* 'obj ' */
#define TYPE_LONG 0x6C6F6E67u     /* 'long' */
#define TYPE_SHORT 0x73686F72u    /* 'shor' */
#define TYPE_TYPE 0x74797065u     /* 'type' */
#define TYPE_ENUM 0x656E756Du     /* 'enum' */
#define TYPE_KEYWORD 0x6B657977u  /* 'keyw' */
#define TYPE_PSN 0x70736E20u      /* 'psn ' */

#define KEY_CLASS 0x6576636Cu       /* keyEventClassAttr 'evcl' */
#define KEY_ID 0x65766964u          /* keyEventIDAttr 'evid' */
#define KEY_RETURN_ID 0x72746964u   /* keyReturnIDAttr 'rtid' */
#define KEY_TRANSACTION 0x7472616Eu /* keyTransactionIDAttr 'tran' */
#define KEY_ADDRESS 0x61646472u     /* keyAddressAttr 'addr' */
#define KEY_WANT 0x77616E74u        /* keyAEDesiredClass 'want' */
#define KEY_FROM 0x66726F6Du        /* keyAEContainer 'from' */
#define KEY_FORM 0x666F726Du        /* keyAEKeyForm 'form' */
#define KEY_SELD 0x73656C64u        /* keyAEKeyData 'seld' */

#define CLASS_AE 0x61657674u /* kCoreEventClass 'aevt' */
#define ID_ANSWER 0x616E7372u /* kAEAnswer 'ansr' */

#define ERR_COERCION_FAIL (-1700)
#define ERR_DESC_NOT_FOUND (-1701)
#define ERR_WRONG_DATA_TYPE (-1703)
#define ERR_NOT_AE_DESC (-1704)
#define ERR_BAD_LIST_ITEM (-1705)
#define ERR_EVENT_NOT_HANDLED (-1708)
#define ERR_NO_SUCH_OBJECT (-1728)
#define ERR_MEM_FULL (-108)
#define AUTO_RETURN_ID (-1)
#define SEND_REPLY_MASK 3 /* the reply mode: kAENoReply, kAEQueueReply or kAEWaitReply */
#define SEND_NO_REPLY 1
#define CURRENT_PROCESS 2 /* kCurrentProcess, the low word of a PSN */

typedef struct {
    uint32_t key;  /* its keyword; 0 in a list */
    uint32_t desc; /* its descriptor, an index into A.d */
} ae_item;

typedef struct {
    uint8_t *data;   /* a plain descriptor's bytes */
    ae_item *items;  /* a list's, a record's or an Apple Event's parameters */
    ae_item *attrs;  /* an Apple Event's attributes */
    uint32_t type, len, nitems, nattrs;
    uint32_t used, aggregate;
} ae_store;

static struct {
    ae_store d[MAX_DESCS];
    int32_t next_return_id;
} A;

/* A snapshot's walk (snapshot.h): every descriptor's bytes and items,
   which it owns. */
static void snap_walk(snapshot_walker *w) {
    if (snapshot_loading(w))
        for (int i = 0; i < MAX_DESCS; i++) {
            free(A.d[i].data);
            free(A.d[i].items);
            free(A.d[i].attrs);
        }
    snapshot_bytes(w, &A, sizeof A);
    SNAPSHOT_PADDING(w, A, next_return_id);
    for (int i = 0; i < MAX_DESCS; i++) {
        ae_store *s = &A.d[i];
        SNAPSHOT_OWNED(w, s->data, s->len);
        SNAPSHOT_OWNED(w, s->items, s->nitems);
        SNAPSHOT_OWNED(w, s->attrs, s->nattrs);
        for (uint32_t k = 0; k < s->nitems; k++)
            snapshot_check(w, s->items[k].desc < MAX_DESCS, "descriptor %d's item %u", i, k);
        for (uint32_t k = 0; k < s->nattrs; k++)
            snapshot_check(w, s->attrs[k].desc < MAX_DESCS, "descriptor %d's attribute %u", i, k);
    }
}
SNAPSHOT_STATE_WALK(A, snap_walk)

/* ---- the descriptors the shim keeps ---- */

static uint32_t store_new(uint32_t type, const void *data, uint32_t len, bool aggregate) {
    for (uint32_t i = 0; i < MAX_DESCS; i++)
        if (!A.d[i].used) {
            ae_store *s = &A.d[i];
            *s = (ae_store){.type = type, .len = len, .used = 1, .aggregate = aggregate};
            if (len) {
                s->data = malloc(len);
                if (!s->data)
                    fatal("out of memory");
                memcpy(s->data, data, len);
            }
            return i;
        }
    trap_crash("more than %d Apple Event descriptors", MAX_DESCS);
}

static void store_free(uint32_t i) {
    ae_store *s = &A.d[i];
    for (uint32_t k = 0; k < s->nitems; k++)
        store_free(s->items[k].desc);
    for (uint32_t k = 0; k < s->nattrs; k++)
        store_free(s->attrs[k].desc);
    free(s->data);
    free(s->items);
    free(s->attrs);
    memset(s, 0, sizeof *s);
}

static ae_item *copy_items(const ae_item *from, uint32_t n);

static uint32_t store_dup(uint32_t i) {
    ae_store *s = &A.d[i];
    uint32_t j = store_new(s->type, s->data, s->len, s->aggregate);
    ae_item *items = copy_items(A.d[i].items, A.d[i].nitems);
    ae_item *attrs = copy_items(A.d[i].attrs, A.d[i].nattrs);
    A.d[j].items = items;
    A.d[j].nitems = A.d[i].nitems;
    A.d[j].attrs = attrs;
    A.d[j].nattrs = A.d[i].nattrs;
    return j;
}

static ae_item *copy_items(const ae_item *from, uint32_t n) {
    if (!n)
        return NULL;
    ae_item *to = malloc(n * sizeof *to);
    if (!to)
        fatal("out of memory");
    memcpy(to, from, n * sizeof *to);
    for (uint32_t k = 0; k < n; k++)
        to[k].desc = store_dup(from[k].desc);
    return to;
}

/* The descriptor an AEDesc's dataHandle names, or -1 for none. */
static int32_t store_of(const char *call, uint32_t desc) {
    uint32_t h = gm_r32(desc + 4);
    if (!h)
        return -1;
    uint32_t i = h - AE_TAG_BASE - 1;
    if (h < AE_TAG_BASE + 1 || i >= MAX_DESCS || !A.d[i].used)
        trap_crash("%s: 0x%08x at 0x%08x is not an Apple Event descriptor", call, h, desc);
    return (int32_t)i;
}

static void write_desc(uint32_t desc, uint32_t type, int32_t store) {
    gm_w32(desc, type);
    gm_w32(desc + 4, store < 0 ? 0 : AE_TAG_BASE + 1 + (uint32_t)store);
}

/* The AEDesc at desc as a descriptor the shim keeps: its own, or a new
   one made from its type alone (a null descriptor, or the events.c fake). */
static uint32_t as_store(const char *call, uint32_t desc) {
    int32_t i = store_of(call, desc);
    return i >= 0 ? store_dup((uint32_t)i) : store_new(gm_r32(desc), NULL, 0, false);
}

void ae_new_null(uint32_t desc) { write_desc(desc, AE_TYPE_NULL, -1); }

void ae_dispose(uint32_t desc) {
    int32_t i = store_of("AEDisposeDesc", desc);
    if (i >= 0)
        store_free((uint32_t)i);
    ae_new_null(desc);
}

/* ---- items ---- */

static void add_item(uint32_t *n, ae_item **items, uint32_t key, uint32_t desc) {
    ae_item *grown = realloc(*items, (*n + 1) * sizeof *grown);
    if (!grown)
        fatal("out of memory");
    grown[*n] = (ae_item){key, desc};
    *items = grown;
    (*n)++;
}

/* Sets key in s's parameters (or attributes) to the descriptor d, which
   it takes, replacing one it had. */
static void put_keyed(uint32_t s, bool attr, uint32_t key, uint32_t d) {
    ae_store *x = &A.d[s];
    uint32_t *n = attr ? &x->nattrs : &x->nitems;
    ae_item **items = attr ? &x->attrs : &x->items;
    for (uint32_t k = 0; k < *n; k++)
        if ((*items)[k].key == key) {
            store_free((*items)[k].desc);
            (*items)[k].desc = d;
            return;
        }
    add_item(n, items, key, d);
}

static int32_t find_keyed(uint32_t s, bool attr, uint32_t key) {
    ae_store *x = &A.d[s];
    uint32_t n = attr ? x->nattrs : x->nitems;
    ae_item *items = attr ? x->attrs : x->items;
    for (uint32_t k = 0; k < n; k++)
        if (items[k].key == key)
            return (int32_t)items[k].desc;
    return -1;
}

static void put_value(uint32_t s, bool attr, uint32_t key, uint32_t type, uint32_t v) {
    uint8_t b[4];
    wr_be32(b, v);
    put_keyed(s, attr, key, store_new(type, b, 4, false));
}

/* ---- coercion ---- */

static bool four_byte(uint32_t t) { return t == TYPE_TYPE || t == TYPE_ENUM || t == TYPE_KEYWORD || t == TYPE_LONG; }

/* A copy of descriptor i as type to, or -1 if it can't be one. */
static int32_t coerce(uint32_t i, uint32_t to) {
    ae_store *s = &A.d[i];
    if (to == TYPE_WILD || to == s->type)
        return (int32_t)store_dup(i);
    if (s->aggregate)
        return -1;
    uint8_t b[4];
    if (four_byte(to) && four_byte(s->type) && s->len == 4)
        return (int32_t)store_new(to, s->data, 4, false);
    if (to == TYPE_LONG && s->type == TYPE_SHORT && s->len == 2) {
        wr_be32(b, (uint32_t)(int32_t)(int16_t)rd_be16(s->data));
        return (int32_t)store_new(to, b, 4, false);
    }
    if (to == TYPE_SHORT && s->type == TYPE_LONG && s->len == 4) {
        int32_t v = (int32_t)rd_be32(s->data);
        if (v < -32768 || v > 32767)
            return -1;
        wr_be16(b, (uint16_t)v);
        return (int32_t)store_new(to, b, 2, false);
    }
    return -1;
}

/* Writes descriptor i, coerced to type, as an AEDesc at out (null and an
   error if it can't be). Returns an OSErr. */
static int16_t give(int32_t i, uint32_t type, uint32_t out) {
    if (i < 0) {
        ae_new_null(out);
        return ERR_DESC_NOT_FOUND;
    }
    int32_t c = coerce((uint32_t)i, type);
    if (c < 0) {
        ae_new_null(out);
        return ERR_COERCION_FAIL;
    }
    write_desc(out, A.d[c].type, c);
    return 0;
}

/* Copies descriptor i, coerced to type, into a buffer: GetParamPtr's and
   GetAttributePtr's out parameters (typeCode, dataPtr, maxSize,
   actualSize; typeCode and actualSize may be NULL). Returns an OSErr. */
static int16_t give_ptr(int32_t i, uint32_t type, uint32_t type_out, uint32_t data, uint32_t max,
                        uint32_t size_out) {
    if (i < 0)
        return ERR_DESC_NOT_FOUND;
    int32_t c = coerce((uint32_t)i, type);
    if (c < 0)
        return ERR_COERCION_FAIL;
    ae_store *s = &A.d[c];
    if (s->aggregate) {
        store_free((uint32_t)c);
        return ERR_WRONG_DATA_TYPE;
    }
    if (type_out)
        gm_w32(type_out, s->type);
    if (size_out)
        gm_w32(size_out, s->len);
    uint32_t n = s->len < max ? s->len : max;
    if (n)
        memcpy(gm_ptr(data, n), s->data, n);
    store_free((uint32_t)c);
    return 0;
}

/* ---- Apple Events ---- */

static uint32_t new_event(uint32_t cls, uint32_t id, int32_t address, int32_t return_id, int32_t transaction) {
    uint32_t s = store_new(AE_TYPE_APPLE_EVENT, NULL, 0, true);
    put_value(s, true, KEY_CLASS, TYPE_TYPE, cls);
    put_value(s, true, KEY_ID, TYPE_TYPE, id);
    if (return_id == AUTO_RETURN_ID)
        return_id = ++A.next_return_id;
    uint8_t b[4];
    wr_be16(b, (uint16_t)return_id);
    put_keyed(s, true, KEY_RETURN_ID, store_new(TYPE_SHORT, b, 2, false));
    put_value(s, true, KEY_TRANSACTION, TYPE_LONG, (uint32_t)transaction);
    if (address < 0) {
        uint8_t psn[8] = {0, 0, 0, 0, 0, 0, 0, CURRENT_PROCESS};
        address = (int32_t)store_new(TYPE_PSN, psn, 8, false);
    }
    put_keyed(s, true, KEY_ADDRESS, (uint32_t)address);
    return s;
}

void ae_new_apple_event(uint32_t desc, uint32_t event_class, uint32_t event_id) {
    write_desc(desc, AE_TYPE_APPLE_EVENT, (int32_t)new_event(event_class, event_id, -1, AUTO_RETURN_ID, 0));
}

static uint32_t attr_value(uint32_t s, uint32_t key) {
    int32_t i = find_keyed(s, true, key);
    return i >= 0 && A.d[i].len == 4 ? rd_be32(A.d[i].data) : 0;
}

/* ---- the calls ---- */

static void ret(int16_t err) { trap_return((uint32_t)(int32_t)err); }

/* The keyed descriptor (a record or an Apple Event) at desc, or crashes. */
static uint32_t need_keyed(const char *call, uint32_t desc) {
    int32_t i = store_of(call, desc);
    if (i < 0 || !A.d[i].aggregate)
        trap_crash("%s: the descriptor at 0x%08x has no items", call, desc);
    return (uint32_t)i;
}

/* AECreateDesc(DescType, const void *dataPtr, Size, AEDesc *result) */
static void h_ae_create_desc(void) {
    uint32_t len = trap_arg(2);
    const void *data = len ? gm_ptr(trap_arg(1), len) : NULL;
    write_desc(trap_arg(3), trap_arg(0), (int32_t)store_new(trap_arg(0), data, len, false));
    ret(0);
}

static void h_ae_dispose_desc(void) {
    ae_dispose(trap_arg(0));
    ret(0);
}

static void h_ae_duplicate_desc(void) {
    int32_t i = store_of("AEDuplicateDesc", trap_arg(0));
    write_desc(trap_arg(1), gm_r32(trap_arg(0)), i < 0 ? -1 : (int32_t)store_dup((uint32_t)i));
    ret(0);
}

static void h_ae_coerce_desc(void) {
    uint32_t s = as_store("AECoerceDesc", trap_arg(0));
    ret(give((int32_t)s, trap_arg(1), trap_arg(2)));
    store_free(s);
}

static void h_ae_get_desc_data_size(void) {
    int32_t i = store_of("AEGetDescDataSize", trap_arg(0));
    trap_return(i < 0 || A.d[i].aggregate ? 0 : A.d[i].len);
}

/* AEGetDescData(const AEDesc *, void *dataPtr, Size maximumSize) */
static void h_ae_get_desc_data(void) {
    int32_t i = store_of("AEGetDescData", trap_arg(0));
    if (i >= 0 && A.d[i].aggregate) {
        ret(ERR_WRONG_DATA_TYPE);
        return;
    }
    uint32_t n = i < 0 ? 0 : A.d[i].len < trap_arg(2) ? A.d[i].len : trap_arg(2);
    if (n)
        memcpy(gm_ptr(trap_arg(1), n), A.d[i].data, n);
    ret(0);
}

/* AECreateList(factoringPtr, factoredSize, Boolean isRecord, AEDescList *) */
static void h_ae_create_list(void) {
    uint32_t type = (trap_arg(2) & 0xFF) ? TYPE_RECORD : TYPE_LIST;
    write_desc(trap_arg(3), type, (int32_t)store_new(type, NULL, 0, true));
    ret(0);
}

static void h_ae_count_items(void) {
    int32_t i = store_of("AECountItems", trap_arg(0));
    if (i < 0 || !A.d[i].aggregate) {
        ret(ERR_WRONG_DATA_TYPE);
        return;
    }
    gm_w32(trap_arg(1), A.d[i].nitems);
    ret(0);
}

/* AEPutDesc(AEDescList *, long index, const AEDesc *): index 0, or one past
   the last, appends. */
static void h_ae_put_desc(void) {
    uint32_t list = need_keyed("AEPutDesc", trap_arg(0));
    int32_t index = (int32_t)trap_arg(1);
    ae_store *s = &A.d[list];
    if (index < 0 || (uint32_t)index > s->nitems + 1) {
        ret(ERR_BAD_LIST_ITEM);
        return;
    }
    uint32_t d = as_store("AEPutDesc", trap_arg(2));
    if (index == 0 || (uint32_t)index == s->nitems + 1) {
        add_item(&A.d[list].nitems, &A.d[list].items, 0, d);
    } else {
        store_free(s->items[index - 1].desc);
        s->items[index - 1].desc = d;
    }
    ret(0);
}

/* AEGetNthDesc(list, long index, DescType desiredType, AEKeyword *, AEDesc *result) */
static void h_ae_get_nth_desc(void) {
    uint32_t list = need_keyed("AEGetNthDesc", trap_arg(0));
    int32_t index = (int32_t)trap_arg(1);
    ae_store *s = &A.d[list];
    if (index < 1 || (uint32_t)index > s->nitems) {
        ae_new_null(trap_arg(4));
        ret(ERR_BAD_LIST_ITEM);
        return;
    }
    if (trap_arg(3))
        gm_w32(trap_arg(3), s->items[index - 1].key);
    ret(give((int32_t)s->items[index - 1].desc, trap_arg(2), trap_arg(4)));
}

/* AECreateAppleEvent(AEEventClass, AEEventID, const AEAddressDesc *target,
   AEReturnID, AETransactionID, AppleEvent *result) */
static void h_ae_create_apple_event(void) {
    int32_t address = trap_arg(2) ? (int32_t)as_store("AECreateAppleEvent", trap_arg(2)) : -1;
    uint32_t s = new_event(trap_arg(0), trap_arg(1), address, (int16_t)trap_arg(3), (int32_t)trap_arg(4));
    write_desc(trap_arg(5), AE_TYPE_APPLE_EVENT, (int32_t)s);
    ret(0);
}

static void h_ae_put_param_desc(void) {
    uint32_t s = need_keyed("AEPutParamDesc", trap_arg(0));
    put_keyed(s, false, trap_arg(1), as_store("AEPutParamDesc", trap_arg(2)));
    ret(0);
}

/* AEPutParamPtr(AppleEvent *, AEKeyword, DescType, const void *, Size) */
static void h_ae_put_param_ptr(void) {
    uint32_t s = need_keyed("AEPutParamPtr", trap_arg(0));
    uint32_t len = trap_arg(4);
    put_keyed(s, false, trap_arg(1), store_new(trap_arg(2), len ? gm_ptr(trap_arg(3), len) : NULL, len, false));
    ret(0);
}

/* A parameter or attribute of the descriptor at desc, or -1: one without
   items (the events.c fake, an 'aevt' with no data) has none. */
static int32_t lookup(const char *call, uint32_t desc, bool attr, uint32_t key) {
    int32_t s = store_of(call, desc);
    return s >= 0 && A.d[s].aggregate ? find_keyed((uint32_t)s, attr, key) : -1;
}

/* AEGetParamDesc(const AppleEvent *, AEKeyword, DescType desiredType, AEDesc *result) */
static void h_ae_get_param_desc(void) {
    ret(give(lookup("AEGetParamDesc", trap_arg(0), false, trap_arg(1)), trap_arg(2), trap_arg(3)));
}

/* AEGetParamPtr(ae, key, desiredType, DescType *typeCode, void *dataPtr,
   Size maximumSize, Size *actualSize) */
static void h_ae_get_param_ptr(void) {
    ret(give_ptr(lookup("AEGetParamPtr", trap_arg(0), false, trap_arg(1)), trap_arg(2), trap_arg(3),
                 trap_arg(4), trap_arg(5), trap_arg(6)));
}

/* The same for an attribute. */
static void h_ae_get_attribute_ptr(void) {
    ret(give_ptr(lookup("AEGetAttributePtr", trap_arg(0), true, trap_arg(1)), trap_arg(2), trap_arg(3),
                 trap_arg(4), trap_arg(5), trap_arg(6)));
}

/* AESend(const AppleEvent *, AppleEvent *reply, AESendMode, AESendPriority,
   long timeOutInTicks, idleProc, filterProc): straight to this program's
   handler. */
static void h_ae_send(void) {
    uint32_t ev = trap_arg(0), reply = trap_arg(1), mode = trap_arg(2);
    uint32_t s = need_keyed("AESend", ev);
    bool no_reply = (mode & SEND_REPLY_MASK) == SEND_NO_REPLY;
    uint32_t cls = attr_value(s, KEY_CLASS), id = attr_value(s, KEY_ID);
    if (reply) {
        if (no_reply)
            ae_new_null(reply);
        else
            write_desc(reply, AE_TYPE_APPLE_EVENT,
                       (int32_t)new_event(CLASS_AE, ID_ANSWER, -1, (int16_t)AUTO_RETURN_ID, 0));
    }
    uint32_t handler, refcon;
    if (!misc_ae_handler(cls, id, &handler, &refcon)) {
        ret(ERR_EVENT_NOT_HANDLED);
        return;
    }
    uint32_t null_reply = 0;
    if (!reply || no_reply) {
        null_reply = mm_new_ptr(8, true);
        if (!null_reply)
            trap_crash("AESend: out of guest memory");
        ae_new_null(null_reply);
    }
    uint32_t args[3] = {ev, null_reply ? null_reply : reply, refcon};
    int16_t err = (int16_t)guest_call(handler, 3, args);
    if (null_reply) {
        ae_dispose(null_reply);
        mm_dispose_ptr(null_reply);
    }
    ret(err);
}

/* AEManagerInfo(AEKeyword, long *result): no recorders, nothing pending. */
static void h_ae_manager_info(void) {
    gm_w32(trap_arg(1), 0);
    ret(0);
}

static void h_ok(void) { ret(0); }
static void h_upp(void) { trap_return(trap_arg(0)); }

static void h_ae_resolve(void) {
    ae_new_null(trap_arg(2));
    ret(ERR_NO_SUCH_OBJECT);
}

/* CreateObjSpecifier(DescType desiredClass, AEDesc *container, DescType
   keyForm, AEDesc *keyData, Boolean disposeInputs, AEDesc *result) */
static void h_create_obj_specifier(void) {
    uint32_t s = store_new(TYPE_OBJECT, NULL, 0, true);
    put_value(s, false, KEY_WANT, TYPE_TYPE, trap_arg(0));
    put_keyed(s, false, KEY_FROM, as_store("CreateObjSpecifier", trap_arg(1)));
    put_value(s, false, KEY_FORM, TYPE_ENUM, trap_arg(2));
    put_keyed(s, false, KEY_SELD, as_store("CreateObjSpecifier", trap_arg(3)));
    if (trap_arg(4) & 0xFF) {
        ae_dispose(trap_arg(1));
        ae_dispose(trap_arg(3));
    }
    write_desc(trap_arg(5), TYPE_OBJECT, (int32_t)s);
    ret(0);
}

static void h_create_offset_descriptor(void) {
    uint8_t b[4];
    wr_be32(b, trap_arg(0));
    write_desc(trap_arg(1), TYPE_LONG, (int32_t)store_new(TYPE_LONG, b, 4, false));
    ret(0);
}

void ae_register(void) {
    trap_register("AECreateDesc", h_ae_create_desc);
    trap_register("AEDisposeDesc", h_ae_dispose_desc);
    trap_register("AEDuplicateDesc", h_ae_duplicate_desc);
    trap_register("AECoerceDesc", h_ae_coerce_desc);
    trap_register("AEGetDescDataSize", h_ae_get_desc_data_size);
    trap_register("AEGetDescData", h_ae_get_desc_data);
    trap_register("AECreateList", h_ae_create_list);
    trap_register("AECountItems", h_ae_count_items);
    trap_register("AEPutDesc", h_ae_put_desc);
    trap_register("AEGetNthDesc", h_ae_get_nth_desc);
    trap_register("AECreateAppleEvent", h_ae_create_apple_event);
    trap_register("AEPutParamDesc", h_ae_put_param_desc);
    trap_register("AEPutParamPtr", h_ae_put_param_ptr);
    trap_register("AEGetParamDesc", h_ae_get_param_desc);
    trap_register("AEGetParamPtr", h_ae_get_param_ptr);
    trap_register("AEGetAttributePtr", h_ae_get_attribute_ptr);
    trap_register("AESend", h_ae_send);
    trap_register("AEManagerInfo", h_ae_manager_info);
    trap_register("AEObjectInit", h_ok);
    trap_register("AESetObjectCallbacks", h_ok);
    trap_register("AEInstallObjectAccessor", h_ok);
    trap_register("AEResolve", h_ae_resolve);
    trap_register("CreateObjSpecifier", h_create_obj_specifier);
    trap_register("CreateOffsetDescriptor", h_create_offset_descriptor);
    trap_register("NewOSLAccessorUPP", h_upp);
    trap_register("NewOSLCompareUPP", h_upp);
    trap_register("NewOSLCountUPP", h_upp);
    trap_register("NewOSLDisposeTokenUPP", h_upp);
    trap_register("NewOSLGetMarkTokenUPP", h_upp);
    trap_register("NewOSLMarkUPP", h_upp);
    trap_register("NewOSLAdjustMarksUPP", h_upp);
    trap_register("NewOSLGetErrDescUPP", h_upp);
}
