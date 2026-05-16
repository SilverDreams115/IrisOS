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
    uint8_t        give_console; /* 1 = forward console channel during bootstrap */
    uint8_t        give_kbd;     /* 1 = forward kbd service channel during bootstrap */
    uint8_t        give_vfs;     /* 1 = forward vfs service+reply channels during bootstrap */
    uint8_t        reserved[1];
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
        .give_console = 1u,
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
        .give_console = 1u,
        .give_kbd = 1u,
        .give_vfs = 1u,
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
