#ifndef LOG_H
#define LOG_H

#include "rayforce.h"

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

// Logging macros that are removed by the compiler when logging is disabled
#define LOG_TRACE(fmt, ...) log_internal(LOG_LEVEL_TRACE, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...) log_internal(LOG_LEVEL_DEBUG, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...) log_internal(LOG_LEVEL_INFO, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...) log_internal(LOG_LEVEL_WARN, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) log_internal(LOG_LEVEL_ERROR, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

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

#endif

#endif  // LOG_H