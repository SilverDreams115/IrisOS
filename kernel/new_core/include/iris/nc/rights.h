#ifndef IRIS_NC_RIGHTS_H
#define IRIS_NC_RIGHTS_H

#include <stdint.h>

typedef uint32_t iris_rights_t;

#define RIGHT_NONE        ((iris_rights_t)0x00000000)
#define RIGHT_READ        ((iris_rights_t)0x00000001)
#define RIGHT_WRITE       ((iris_rights_t)0x00000002)
#define RIGHT_DUPLICATE   ((iris_rights_t)0x00000004)
#define RIGHT_TRANSFER    ((iris_rights_t)0x00000008)  /* move handle to another process */
#define RIGHT_WAIT        ((iris_rights_t)0x00000010)
#define RIGHT_MANAGE      ((iris_rights_t)0x00000040)

/* Flag de solicitud para duplicación — nunca almacenar en HandleEntry.rights */
#define RIGHT_SAME_RIGHTS ((iris_rights_t)0x80000000)

/*
 * Invariantes:
 *   - Derechos solo pueden reducirse al duplicar, nunca aumentarse
 *   - RIGHT_TRANSFER no aplicable hasta Channel v1.1
 *   - Cerrar un handle nunca requiere ningún derecho
 *   - RIGHT_SAME_RIGHTS es flag de operación, no estado persistente
 *   - Ninguna ruta del kernel escala rights de una HandleEntry existente
 */

static inline int rights_check(iris_rights_t rights, iris_rights_t required) {
    return (rights & required) == required;
}

/* Retorna intersección — nunca escala */
static inline iris_rights_t rights_reduce(iris_rights_t base,
                                          iris_rights_t requested) {
    if (requested & RIGHT_SAME_RIGHTS) return base & ~RIGHT_SAME_RIGHTS;
    return base & requested;
}

#endif
