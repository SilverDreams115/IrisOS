#ifndef SVCMGR_SERVICE_CATALOG_H
#define SVCMGR_SERVICE_CATALOG_H

#include <stdint.h>
#include <iris/nc/rights.h>
#include <iris/svcmgr_proto.h>

#define IRIS_BOOTSTRAP_SUPERVISOR_IMAGE "svcmgr"
#define IRIS_SERVICE_IMAGE_KBD          "kbd"
#define IRIS_SERVICE_IMAGE_VFS          "vfs"
#define IRIS_SERVICE_IMAGE_SH           "sh"
#define IRIS_SERVICE_RUNTIME_SLOT_COUNT 4u

struct iris_service_catalog_entry {
    const char    *image_name;
    uint32_t       service_id;
    uint32_t       service_endpoint;
    uint32_t       reply_endpoint;
    uint8_t        irq_num;
    uint8_t        autostart;
    uint8_t        restart_on_exit;
    uint8_t        restart_limit;
    iris_rights_t  child_service_rights;
    iris_rights_t  child_reply_rights;
    iris_rights_t  client_service_rights;
    iris_rights_t  client_reply_rights;
    uint16_t       ioport_base;
    uint16_t       ioport_count;
    uint8_t        give_console;    /* 1 = forward console channel during bootstrap */
    uint8_t        give_spawn_cap;  /* 1 = forward spawn cap so service can load initrd VMOs */
    uint8_t        give_irqcap;     /* 1 = forward KIrqCap so service can call SYS_IRQ_ACK */
    uint8_t        irq_notify;      /* 1 = route the IRQ to a KNotification owned by svcmgr
                                     *     (Fase 7.6) instead of the legacy service KChannel;
                                     *     the WAIT side reaches the child at bootstrap as
                                     *     SVCMGR_BOOTSTRAP_KIND_IRQ_NOTIFY. */
    uint8_t        own_service_ep;  /* 1 = svcmgr creates a KEndpoint for this service,
                                     *     sends the recv side at bootstrap (kind 0x21)
                                     *     and publishes it as "<image_name>.ep" (Fase 7.1) */
    uint8_t        endpoint_only;   /* 1 = no legacy KChannel service/reply pair: svcmgr
                                     *     creates neither channel and bootstrap sends no
                                     *     SERVICE/REPLY kinds; lookups by the service's
                                     *     endpoint ids fail.
                                     *     With own_service_ep=1: an endpoint server
                                     *     (Fase 7.5: vfs — ready when ep_h exists).
                                     *     With own_service_ep=0: a pure CPtr-first
                                     *     client (Fase 8: sh — empty bootstrap bag,
                                     *     ready when proc_h is alive). */
};

static const struct iris_service_catalog_entry g_iris_service_catalog[] = {
    {
        .image_name = IRIS_SERVICE_IMAGE_KBD,
        .service_id = SVCMGR_SERVICE_KBD,
        .service_endpoint = SVCMGR_ENDPOINT_KBD,
        .reply_endpoint = SVCMGR_ENDPOINT_KBD_REPLY,
        .irq_num = 1u,
        .autostart = 1u,
        .restart_on_exit = 1u,
        .restart_limit = 3u,
        .child_service_rights = RIGHT_READ,
        .child_reply_rights = RIGHT_WRITE,
        .client_service_rights = RIGHT_WRITE,
        .client_reply_rights = RIGHT_READ,
        .ioport_base = 0x60u,
        .ioport_count = 5u,
        .give_console = 0u,
        .give_irqcap = 1u,
        /* Fase 7.4: kbd owns a KEndpoint ("kbd.ep"); sh pulls key events
         * through it. Fase 7.6: the IRQ reaches kbd as a KNotification.
         * Fase 13/Track I: the legacy service/reply KChannel pair is retired —
         * kbd is endpoint-only (HELLO/STATUS/SUBSCRIBE gone). */
        .own_service_ep = 1u,
        .irq_notify = 1u,
        .endpoint_only = 1u,
    },
    {
        .image_name = IRIS_SERVICE_IMAGE_VFS,
        .service_id = SVCMGR_SERVICE_VFS,
        .service_endpoint = SVCMGR_ENDPOINT_VFS,
        .reply_endpoint = SVCMGR_ENDPOINT_VFS_REPLY,
        .irq_num = 0xFFu,
        .autostart = 1u,
        .restart_on_exit = 1u,
        .restart_limit = 3u,
        .child_service_rights = RIGHT_READ | RIGHT_WRITE,
        .child_reply_rights = RIGHT_WRITE,
        .client_service_rights = RIGHT_WRITE | RIGHT_DUPLICATE,
        .client_reply_rights = RIGHT_READ | RIGHT_DUPLICATE,
        .ioport_base = 0u,
        .ioport_count = 0u,
        .give_console = 0u,  /* Fase 8: vfs logs via IRIS_CPTR_CONSOLE_EP */
        .give_spawn_cap = 1u,
        .own_service_ep = 1u,
        /* Fase 7.5: vfs is endpoint-only — no legacy service/reply channels. */
        .endpoint_only = 1u,
    },
    {
        .image_name = IRIS_SERVICE_IMAGE_SH,
        .service_id = SVCMGR_SERVICE_SH,
        .service_endpoint = SVCMGR_ENDPOINT_SH,
        .reply_endpoint = SVCMGR_ENDPOINT_SH_REPLY,
        .irq_num = 0xFFu,
        .autostart = 1u,
        .restart_on_exit = 0u,
        .restart_limit = 0u,
        .child_service_rights = RIGHT_READ,
        .child_reply_rights = RIGHT_WRITE,
        .client_service_rights = RIGHT_WRITE,
        .client_reply_rights = RIGHT_READ,
        .ioport_base = 0u,
        .ioport_count = 0u,
        .give_console = 0u,
        /* sh reaches VFS only through "vfs.ep" (Fase 7.2) and kbd only
         * through "kbd.ep" (Fase 7.4); the legacy give_vfs/give_kbd
         * forwarding flags were removed in 7.5/7.4.
         * Fase 8: sh is a pure CPtr-first client — endpoint_only without
         * an own endpoint means no legacy channel pair and an empty
         * bootstrap bag; everything sh needs arrives as well-known CSpace
         * slots 1..4. Readiness tracks proc_h (svcmgr_ready_service_count). */
        .endpoint_only = 1u,
    },
};

static inline uint32_t iris_service_catalog_count(void) {
    return (uint32_t)(sizeof(g_iris_service_catalog) / sizeof(g_iris_service_catalog[0]));
}

static inline const struct iris_service_catalog_entry *iris_service_catalog_at(uint32_t index) {
    if (index >= iris_service_catalog_count()) return 0;
    return &g_iris_service_catalog[index];
}

static inline const struct iris_service_catalog_entry *iris_service_catalog_find_by_service_id(uint32_t service_id) {
    for (uint32_t i = 0; i < iris_service_catalog_count(); i++) {
        if (g_iris_service_catalog[i].service_id == service_id)
            return &g_iris_service_catalog[i];
    }
    return 0;
}

static inline const struct iris_service_catalog_entry *iris_service_catalog_find_by_endpoint(uint32_t endpoint,
                                                                                              int *is_reply) {
    for (uint32_t i = 0; i < iris_service_catalog_count(); i++) {
        if (g_iris_service_catalog[i].service_endpoint == endpoint) {
            if (is_reply) *is_reply = 0;
            return &g_iris_service_catalog[i];
        }
        if (g_iris_service_catalog[i].reply_endpoint == endpoint) {
            if (is_reply) *is_reply = 1;
            return &g_iris_service_catalog[i];
        }
    }
    return 0;
}

#endif /* SVCMGR_SERVICE_CATALOG_H */
