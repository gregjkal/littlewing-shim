#include "test.h"

#include "ae.h"
#include "asm.h"
#include "harness.h"
#include "memmgr.h"
#include "misc.h"

static const char *const names[] = {
    "AECreateDesc",      "AEDisposeDesc",      "AEDuplicateDesc",        "AECoerceDesc",
    "AEGetDescDataSize", "AEGetDescData",      "AECreateList",           "AECountItems",
    "AEPutDesc",         "AEGetNthDesc",       "AECreateAppleEvent",     "AEPutParamDesc",
    "AEPutParamPtr",     "AEGetParamDesc",     "AEGetParamPtr",          "AEGetAttributePtr",
    "AESend",            "AEManagerInfo",      "AEObjectInit",           "AESetObjectCallbacks",
    "AEInstallObjectAccessor", "AEResolve",    "CreateObjSpecifier",     "CreateOffsetDescriptor",
    "NewOSLAccessorUPP", "AEInstallEventHandler", "NewAEEventHandlerUPP",
};

#define FOURCC_(s) FOURCC((s)[0], (s)[1], (s)[2], (s)[3])

static void setup(void) {
    harness_init_direct(names, sizeof names / sizeof names[0]);
    mm_init();
    misc_init();
    misc_register();
    ae_register();
}

static uint32_t desc(void) { return scratch(8); }

static uint32_t new_long(int32_t v) {
    uint32_t d = desc(), p = scratch(4);
    gm_w32(p, (uint32_t)v);
    if (call_import("AECreateDesc", 4, FOURCC_("long"), p, 4u, d) != 0)
        fatal("AECreateDesc failed");
    return d;
}

/* The 4-byte value of a plain descriptor. */
static uint32_t value_of(uint32_t d) {
    uint32_t p = scratch(4);
    call_import("AEGetDescData", 3, d, p, 4u);
    return gm_r32(p);
}

TEST(ae_plain_descriptors) {
    setup();
    uint32_t d = new_long(1234);
    CHECK_EQ(gm_r32(d), FOURCC_("long"));
    CHECK(gm_r32(d + 4) > AE_TAG_BASE && gm_r32(d + 4) < AE_TAG_LIMIT);
    CHECK_EQ(call_import("AEGetDescDataSize", 1, d), 4);
    CHECK_EQ(value_of(d), 1234);
    uint32_t copy = desc();
    CHECK_EQ(call_import("AEDuplicateDesc", 2, d, copy), 0);
    CHECK(gm_r32(copy + 4) != gm_r32(d + 4));
    CHECK_EQ(call_import("AEDisposeDesc", 1, d), 0);
    CHECK_EQ(gm_r32(d), AE_TYPE_NULL);
    CHECK_EQ(gm_r32(d + 4), 0);
    CHECK_EQ(value_of(copy), 1234);

    uint32_t as = desc();
    CHECK_EQ(call_import("AECoerceDesc", 3, copy, FOURCC_("shor"), as), 0);
    CHECK_EQ(gm_r32(as), FOURCC_("shor"));
    CHECK_EQ(call_import("AEGetDescDataSize", 1, as), 2);
    CHECK_EQ((int16_t)call_import("AECoerceDesc", 3, copy, FOURCC_("TEXT"), as), -1700);
    CHECK_EQ(gm_r32(as), AE_TYPE_NULL);
}

TEST(ae_lists_hold_copies) {
    setup();
    uint32_t list = desc(), count = scratch(4), key = scratch(4), out = desc();
    CHECK_EQ(call_import("AECreateList", 4, 0u, 0u, 0u, list), 0);
    CHECK_EQ(gm_r32(list), FOURCC_("list"));
    uint32_t a = new_long(1), b = new_long(2);
    CHECK_EQ(call_import("AEPutDesc", 3, list, 0u, a), 0);
    CHECK_EQ(call_import("AEPutDesc", 3, list, 2u, b), 0);
    call_import("AEDisposeDesc", 1, a); /* the list has its own */
    CHECK_EQ(call_import("AECountItems", 2, list, count), 0);
    CHECK_EQ(gm_r32(count), 2);
    CHECK_EQ(call_import("AEGetNthDesc", 5, list, 1u, FOURCC_("****"), key, out), 0);
    CHECK_EQ(value_of(out), 1);
    CHECK_EQ(gm_r32(key), 0);
    CHECK_EQ(call_import("AEPutDesc", 3, list, 1u, new_long(7)), 0);
    CHECK_EQ(call_import("AEGetNthDesc", 5, list, 1u, FOURCC_("long"), key, out), 0);
    CHECK_EQ(value_of(out), 7);
    CHECK_EQ((int16_t)call_import("AEGetNthDesc", 5, list, 3u, FOURCC_("****"), key, out), -1705);
    CHECK_EQ((int16_t)call_import("AECountItems", 2, new_long(1), count), -1703);

    uint32_t dup = desc();
    call_import("AEDuplicateDesc", 2, list, dup);
    call_import("AEDisposeDesc", 1, list);
    CHECK_EQ(call_import("AECountItems", 2, dup, count), 0);
    CHECK_EQ(gm_r32(count), 2);
}

TEST(ae_apple_events_have_attributes_and_parameters) {
    setup();
    uint32_t ev = desc(), type = scratch(4), buf = scratch(16), size = scratch(4);
    CHECK_EQ(call_import("AECreateAppleEvent", 6, FOURCC_("aevt"), FOURCC_("odoc"), 0u, 0xFFFFFFFFu, 0u, ev), 0);
    CHECK_EQ(gm_r32(ev), AE_TYPE_APPLE_EVENT);
    CHECK_EQ(call_import("AEGetAttributePtr", 7, ev, FOURCC_("evid"), FOURCC_("type"), type, buf, 4u, size), 0);
    CHECK_EQ(gm_r32(buf), FOURCC_("odoc"));
    CHECK_EQ(gm_r32(size), 4);
    CHECK_EQ(call_import("AEGetAttributePtr", 7, ev, FOURCC_("evcl"), FOURCC_("****"), type, buf, 4u, size), 0);
    CHECK_EQ(gm_r32(buf), FOURCC_("aevt"));
    CHECK_EQ(call_import("AEGetAttributePtr", 7, ev, FOURCC_("rtid"), FOURCC_("long"), type, buf, 4u, size), 0);
    CHECK((int32_t)gm_r32(buf) > 0); /* generated */
    CHECK_EQ((int16_t)call_import("AEGetAttributePtr", 7, ev, FOURCC_("miss"), FOURCC_("****"), type, buf, 4u,
                                  size),
             -1701);

    uint32_t v = scratch(4);
    gm_w32(v, FOURCC_("yes "));
    CHECK_EQ(call_import("AEPutParamPtr", 5, ev, FOURCC_("savo"), FOURCC_("enum"), v, 4u), 0);
    CHECK_EQ(call_import("AEGetParamPtr", 7, ev, FOURCC_("savo"), FOURCC_("type"), type, buf, 16u, size), 0);
    CHECK_EQ(gm_r32(type), FOURCC_("type"));
    CHECK_EQ(gm_r32(buf), FOURCC_("yes "));
    uint32_t got = desc();
    CHECK_EQ(call_import("AEPutParamDesc", 3, ev, FOURCC_("----"), new_long(5)), 0);
    CHECK_EQ(call_import("AEGetParamDesc", 4, ev, FOURCC_("----"), FOURCC_("****"), got), 0);
    CHECK_EQ(value_of(got), 5);
    CHECK_EQ((int16_t)call_import("AEGetParamDesc", 4, ev, FOURCC_("nope"), FOURCC_("****"), got), -1701);
    CHECK_EQ(gm_r32(got), AE_TYPE_NULL);

    /* The shim's own events, for events.c. */
    uint32_t mine = desc();
    ae_new_apple_event(mine, FOURCC_("aevt"), FOURCC_("oapp"));
    CHECK_EQ(call_import("AEGetAttributePtr", 7, mine, FOURCC_("evid"), FOURCC_("type"), type, buf, 4u, size), 0);
    CHECK_EQ(gm_r32(buf), FOURCC_("oapp"));
    CHECK_EQ(call_import("AEGetAttributePtr", 7, mine, FOURCC_("addr"), FOURCC_("****"), type, buf, 16u, size), 0);
    CHECK_EQ(gm_r32(type), FOURCC_("psn "));
    ae_dispose(mine);
    CHECK_EQ(gm_r32(mine), AE_TYPE_NULL);

    /* A desc with no data (events.c's old fake) has no parameters. */
    uint32_t fake = desc();
    gm_w32(fake, AE_TYPE_APPLE_EVENT);
    CHECK_EQ((int16_t)call_import("AEGetParamDesc", 4, fake, FOURCC_("----"), FOURCC_("****"), got), -1701);
}

/* A handler that stores its refcon (r5) at out and returns 0. */
static uint32_t handler_storing_refcon(uint32_t out) {
    uint32_t code = scratch(32);
    uint32_t w[] = {ppc_lis(4, out >> 16), ppc_ori(4, 4, out & 0xFFFF), ppc_stw(5, 0, 4), ppc_addi(3, 0, 0),
                    PPC_BLR};
    for (unsigned i = 0; i < sizeof w / sizeof w[0]; i++)
        gm_w32(code + 4 * i, w[i]);
    return code;
}

TEST(ae_send_goes_to_this_programs_handler) {
    setup();
    uint32_t seen = scratch(4);
    uint32_t h = handler_storing_refcon(seen);
    CHECK_EQ(call_import("AEInstallEventHandler", 5, FOURCC_("misc"), FOURCC_("dosc"),
                         call_import("NewAEEventHandlerUPP", 1, h), 77u, 0u),
             0);
    uint32_t ev = desc(), reply = desc();
    call_import("AECreateAppleEvent", 6, FOURCC_("misc"), FOURCC_("dosc"), 0u, 0xFFFFFFFFu, 0u, ev);
    CHECK_EQ(call_import("AESend", 7, ev, reply, 3u /* kAEWaitReply */, 0u, 0u, 0u, 0u), 0);
    CHECK_EQ(gm_r32(seen), 77);
    CHECK_EQ(gm_r32(reply), AE_TYPE_APPLE_EVENT);
    gm_w32(seen, 0);
    CHECK_EQ(call_import("AESend", 7, ev, reply, 1u /* kAENoReply */, 0u, 0u, 0u, 0u), 0);
    CHECK_EQ(gm_r32(seen), 77);
    CHECK_EQ(gm_r32(reply), AE_TYPE_NULL);
    uint32_t other = desc();
    call_import("AECreateAppleEvent", 6, FOURCC_("misc"), FOURCC_("none"), 0u, 0xFFFFFFFFu, 0u, other);
    CHECK_EQ((int16_t)call_import("AESend", 7, other, 0u, 1u, 0u, 0u, 0u, 0u), -1708);
}

TEST(ae_object_support_is_accepted) {
    setup();
    uint32_t n = scratch(4);
    CHECK_EQ(call_import("AEObjectInit", 0), 0);
    CHECK_EQ(call_import("AEManagerInfo", 2, FOURCC_("recr"), n), 0);
    CHECK_EQ(gm_r32(n), 0);
    CHECK_EQ(call_import("NewOSLAccessorUPP", 1, 0x1234u), 0x1234);
    CHECK_EQ(call_import("AEInstallObjectAccessor", 5, FOURCC_("cwin"), FOURCC_("null"), 0x1234u, 0u, 0u), 0);
    uint32_t null = desc(), key = desc(), spec = desc(), token = desc();
    ae_new_null(null);
    CHECK_EQ(call_import("CreateOffsetDescriptor", 2, 1u, key), 0);
    CHECK_EQ(gm_r32(key), FOURCC_("long"));
    CHECK_EQ(call_import("CreateObjSpecifier", 6, FOURCC_("cwin"), null, FOURCC_("indx"), key, 1u, spec), 0);
    CHECK_EQ(gm_r32(spec), FOURCC_("obj "));
    CHECK_EQ(gm_r32(key), AE_TYPE_NULL); /* disposed */
    uint32_t got = desc(), type = scratch(4), buf = scratch(4), size = scratch(4);
    CHECK_EQ(call_import("AEGetParamPtr", 7, spec, FOURCC_("want"), FOURCC_("type"), type, buf, 4u, size), 0);
    CHECK_EQ(gm_r32(buf), FOURCC_("cwin"));
    CHECK_EQ(call_import("AEGetParamDesc", 4, spec, FOURCC_("seld"), FOURCC_("long"), got), 0);
    CHECK_EQ(value_of(got), 1);
    CHECK_EQ((int16_t)call_import("AEResolve", 3, spec, 0u, token), -1728);
}
