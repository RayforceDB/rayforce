#ifdef DEBUG

#include "log.h"
#include "def.h"
#include "os.h"
#include "string.h"
#include "format.h"
#include <ctype.h>

// Default log level if not specified
static log_level_t current_level = LOG_LEVEL_INFO;
static str_p file_filters = NULL;
static i32_t num_filters = 0;
static b8_t level_initialized = B8_FALSE;

// Parse log level and file filters from environment variable
static void parse_log_config(c8_t* config) {
    str_p level_end = strchr(config, '[');
    str_p files_start = level_end;
    str_p files_end = strchr(config, ']');

    // Parse log level
    if (level_end)
        *level_end = '\0';

    // Convert to uppercase for case-insensitive comparison
    for (str_p p = config; *p; p++)
        *p = toupper(*p);

    if (strcmp(config, "OFF") == 0)
        current_level = LOG_LEVEL_OFF;
    else if (strcmp(config, "ERROR") == 0)
        current_level = LOG_LEVEL_ERROR;
    else if (strcmp(config, "WARN") == 0)
        current_level = LOG_LEVEL_WARN;
    else if (strcmp(config, "INFO") == 0)
        current_level = LOG_LEVEL_INFO;
    else if (strcmp(config, "DEBUG") == 0)
        current_level = LOG_LEVEL_DEBUG;
    else if (strcmp(config, "TRACE") == 0)
        current_level = LOG_LEVEL_TRACE;

    // Parse file filters if present
    if (files_start && files_end) {
        *files_end = '\0';
        files_start++;  // Skip the '['

        // Count number of files
        num_filters = 1;
        for (str_p p = files_start; *p; p++) {
            if (*p == ',')
                num_filters++;
        }

        // Allocate and copy file filters
        file_filters = (str_p)malloc(strlen(files_start) + 1);
        strcpy(file_filters, files_start);

        // Replace commas with null terminators
        for (str_p p = file_filters; *p; p++) {
            if (*p == ',')
                *p = '\0';
        }
    }
}

// Initialize log level from environment variable
static void init_log_level(nil_t) {
    if (!level_initialized) {
        c8_t config[256];
        if (os_get_var("RAYFORCE_LOG_LEVEL", config, sizeof(config)) == 0) {
            parse_log_config(config);
        } else {
            // If environment variable is not set, use default level
            current_level = LOG_LEVEL_INFO;
        }
        level_initialized = B8_TRUE;
    }
}

// Get log level from environment variable
log_level_t log_get_level(nil_t) {
    init_log_level();
    return current_level;
}

// Check if file should be logged
static b8_t should_log_file(lit_p file) {
    if (!file_filters)
        return B8_TRUE;

    // Get just the filename from the full path
    lit_p filename = strrchr(file, '/');
    filename = filename ? filename + 1 : file;

    // Create a copy of the filename
    c8_t filename_copy[256];
    strncpy(filename_copy, filename, sizeof(filename_copy) - 1);
    filename_copy[sizeof(filename_copy) - 1] = '\0';

    // Strip file extension
    str_p ext = strrchr(filename_copy, '.');
    if (ext) {
        *ext = '\0';
    }

    // Check against each filter
    str_p filter = file_filters;
    for (i32_t i = 0; i < num_filters; i++) {
        if (strcmp(filename_copy, filter) == 0) {
            return B8_TRUE;
        }
        filter += strlen(filter) + 1;
    }

    return B8_FALSE;
}

// Get color for log level
static lit_p get_level_color(log_level_t level) {
    switch (level) {
        case LOG_LEVEL_TRACE:
            return CYAN;
        case LOG_LEVEL_DEBUG:
            return BLUE;
        case LOG_LEVEL_INFO:
            return GREEN;
        case LOG_LEVEL_WARN:
            return YELLOW;
        case LOG_LEVEL_ERROR:
            return RED;
        case LOG_LEVEL_OFF:
        default:
            return RESET;
    }
}

// Get level name
static lit_p get_level_name(log_level_t level) {
    switch (level) {
        case LOG_LEVEL_TRACE:
            return "TRACE";
        case LOG_LEVEL_DEBUG:
            return "DEBUG";
        case LOG_LEVEL_INFO:
            return "INFO";
        case LOG_LEVEL_WARN:
            return "WARN";
        case LOG_LEVEL_ERROR:
            return "ERROR";
        case LOG_LEVEL_OFF:
            return "OFF";
        default:
            return "UNKNOWN";
    }
}

// Internal logging function
nil_t log_internal(log_level_t level, lit_p file, i32_t line, lit_p func, lit_p fmt, ...) {
    // Get current log level
    log_level_t current = log_get_level();

    // Filter messages based on log level
    // If current level is OFF (0), show nothing
    // If current level is ERROR (1), show ERROR only
    // If current level is WARN (2), show WARN and above
    // If current level is INFO (3), show INFO and above
    // If current level is DEBUG (4), show DEBUG and above
    // If current level is TRACE (5), show all messages
    if (level > current)
        return;

    // Check file filter
    if (!should_log_file(file))
        return;

    va_list args;
    va_start(args, fmt);

    // Get timestamp
    time_t now = time(NULL);
    struct tm* tm_info = localtime(&now);
    c8_t timestamp[20];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);

    // Get just the filename from the full path
    lit_p filename = strrchr(file, '/');
    filename = filename ? filename + 1 : file;

    // Print the log message with color
    fprintf(stderr, "%s[%s] %s%s:%d %s()%s - %s%s%s - %s", get_level_color(level), timestamp, LIGHT_BLUE, filename,
            line, func, RESET, get_level_color(level), get_level_name(level), RESET, get_level_color(level));
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "%s\n", RESET);

    va_end(args);
}

#endif  // DEBUG