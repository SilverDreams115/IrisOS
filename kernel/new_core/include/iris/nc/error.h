#ifndef IRIS_NC_ERROR_H
#define IRIS_NC_ERROR_H

#include <stdint.h>

typedef int32_t iris_error_t;

enum {
    IRIS_OK                =  0,
    IRIS_ERR_INVALID_ARG   = -1,
    IRIS_ERR_NO_MEMORY     = -2,
    IRIS_ERR_NOT_FOUND     = -3,
    IRIS_ERR_BAD_HANDLE    = -4,
    IRIS_ERR_ACCESS_DENIED = -5,
    IRIS_ERR_TABLE_FULL    = -6,
    IRIS_ERR_ALREADY_EXISTS= -7,
    IRIS_ERR_BUSY          = -8,
    IRIS_ERR_WRONG_TYPE    = -9,
    IRIS_ERR_OVERFLOW      = -10,
    IRIS_ERR_NOT_SUPPORTED = -11,
    IRIS_ERR_CLOSED        = -12,
    IRIS_ERR_WOULD_BLOCK   = -13,
    IRIS_ERR_INTERNAL      = -14,
    IRIS_ERR_TIMED_OUT     = -15,
};

static inline int iris_is_ok(iris_error_t err)  { return err == IRIS_OK; }
static inline int iris_is_err(iris_error_t err) { return err < 0; }

#endif
