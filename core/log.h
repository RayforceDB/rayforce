#ifndef LOG_H
#define LOG_H

#include "rayforce.h"
#include "def.h"

#ifdef DEBUG

typedef enum {
    LOG_LEVEL_OFF,    // Most severe - no logging
    LOG_LEVEL_ERROR,  // Error conditions
    LOG_LEVEL_WARN,   // Warning conditions
    LOG_LEVEL_INFO,   // Informational messages
    LOG_LEVEL_DEBUG,  // Debug-level messages
    LOG_LEVEL_TRACE   // Least severe - trace messages
} log_level_t;

// Get current log level from environment variable
log_level_t log_get_level(nil_t);

// Internal logging function
nil_t log_internal(log_level_t level, lit_p file, i32_t line, lit_p func, lit_p fmt, ...);

// Helper macro for logging with optional object
#define LOG_WITH_OBJ(level, fmt, obj, ...)                                                                            \
    do {                                                                                                              \
        if (obj != NULL) {                                                                                            \
            obj_p _f = obj_fmt((obj), B8_TRUE);                                                                       \
            log_internal(level, __FILE__, __LINE__, __func__, fmt " %.*s", ##__VA_ARGS__, (i32_t)_f->len, AS_C8(_f)); \
            drop_obj(_f);                                                                                             \
        } else {                                                                                                      \
            log_internal(level, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__);                                    \
        }                                                                                                             \
    } while (0)

// Logging macros that are removed by the compiler when logging is disabled
#define LOG_TRACE(fmt, ...) LOG_WITH_OBJ(LOG_LEVEL_TRACE, fmt, NULL, ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...) LOG_WITH_OBJ(LOG_LEVEL_DEBUG, fmt, NULL, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...) LOG_WITH_OBJ(LOG_LEVEL_INFO, fmt, NULL, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...) LOG_WITH_OBJ(LOG_LEVEL_WARN, fmt, NULL, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) LOG_WITH_OBJ(LOG_LEVEL_ERROR, fmt, NULL, ##__VA_ARGS__)

// Logging macros with object
#define LOG_TRACE_OBJ(fmt, obj, ...) LOG_WITH_OBJ(LOG_LEVEL_TRACE, fmt, obj, ##__VA_ARGS__)
#define LOG_DEBUG_OBJ(fmt, obj, ...) LOG_WITH_OBJ(LOG_LEVEL_DEBUG, fmt, obj, ##__VA_ARGS__)
#define LOG_INFO_OBJ(fmt, obj, ...) LOG_WITH_OBJ(LOG_LEVEL_INFO, fmt, obj, ##__VA_ARGS__)
#define LOG_WARN_OBJ(fmt, obj, ...) LOG_WITH_OBJ(LOG_LEVEL_WARN, fmt, obj, ##__VA_ARGS__)
#define LOG_ERROR_OBJ(fmt, obj, ...) LOG_WITH_OBJ(LOG_LEVEL_ERROR, fmt, obj, ##__VA_ARGS__)

#else

// In non-debug mode, all logging is completely removed
typedef i32_t log_level_t;
#define log_get_level() ((log_level_t)0)
#define log_internal(level, file, line, func, fmt, ...) ((nil_t)0)
#define LOG_TRACE(fmt, ...) ((nil_t)0)
#define LOG_DEBUG(fmt, ...) ((nil_t)0)
#define LOG_INFO(fmt, ...) ((nil_t)0)
#define LOG_WARN(fmt, ...) ((nil_t)0)
#define LOG_ERROR(fmt, ...) ((nil_t)0)
#define LOG_TRACE_OBJ(fmt, obj, ...) ((nil_t)0)
#define LOG_DEBUG_OBJ(fmt, obj, ...) ((nil_t)0)
#define LOG_INFO_OBJ(fmt, obj, ...) ((nil_t)0)
#define LOG_WARN_OBJ(fmt, obj, ...) ((nil_t)0)
#define LOG_ERROR_OBJ(fmt, obj, ...) ((nil_t)0)

#endif

#endif  // LOG_H