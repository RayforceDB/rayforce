/*
 *   Copyright (c) 2024 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:
 *
 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 */

#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include <sys/types.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <math.h>
#include "../core/rayforce.h"
#include "../core/format.h"
#include "../core/unary.h"
#include "../core/heap.h"
#include "../core/eval.h"
#include "../core/hash.h"
#include "../core/symbols.h"
#include "../core/string.h"
#include "../core/util.h"
#include "../core/parse.h"
#include "../core/runtime.h"
#include "../core/cmp.h"
#include "../core/eval.h"

#define MAX_SCRIPT_NAME 256
#define MAX_SCRIPT_CONTENT 8192
#define MAX_PARAMS 10
#define MAX_PARAM_LENGTH 256
#define MAX_RESULTS 100
#define DEFAULT_ITERATIONS 1000
#define BENCH_RESULTS_FILE "bench/results.json"
#define BENCH_SCRIPTS_DIR "bench/scripts"
#define BENCH_INIT_SUFFIX ".init"
#define BENCH_PARAM_PREFIX ";;"
#define MAX_OS_INFO 256
#define MAX_CPU_INFO 256
#define MAX_TIMESTAMP 64
#define MAX_GIT_COMMIT 64
#define MAX_VERSION_STR 128
#define MAX_PATH_LEN 512

typedef struct {
    char name[MAX_SCRIPT_NAME];
    char content[MAX_SCRIPT_CONTENT];
    char init_script[MAX_SCRIPT_CONTENT];
    int iterations;
    double expected_time;  // in milliseconds
} bench_script_t;

typedef struct {
    char script_name[MAX_SCRIPT_NAME];
    double min_time;
    double max_time;
    double avg_time;
    double expected_time;  // in milliseconds
    char timestamp[MAX_TIMESTAMP];
    char os_info[MAX_OS_INFO];
    char cpu_info[MAX_CPU_INFO];
    char git_commit[MAX_GIT_COMMIT];
} bench_result_t;

typedef struct {
    bench_result_t results[MAX_RESULTS];
    int result_count;
} bench_results_t;

// Function declarations
void get_system_info(char* os_info, size_t os_size, char* cpu_info, size_t cpu_size);
void get_git_commit(char* commit_hash, size_t hash_size);
void parse_script_params(const char* content, bench_script_t* script);
void run_benchmark(bench_script_t* script, bench_result_t* result);
void load_previous_results(bench_results_t* results);
void save_results(bench_results_t* results);
void compare_and_print_results(bench_result_t* current, bench_result_t* previous);
void print_colored_diff(double current, double previous);
void print_expected_time_diff(double actual, double expected);
void scan_benchmark_scripts(bench_results_t* results);
void process_script_file(const char* filename, bench_results_t* results);
void print_system_info(bench_result_t* result);

// Get system information
void get_system_info(char* os_info, size_t os_size, char* cpu_info, size_t cpu_size) {
    sys_info_t info = sys_info(0);  // Get system info with default thread count

    // Format version string with explicit length checks
    size_t remaining = os_size;
    int written = snprintf(os_info, remaining, "Rayforce %d.%d", info.major_version, info.minor_version);
    if (written > 0 && (size_t)written < remaining) {
        os_info += written;
        remaining -= (size_t)written;
        if (remaining > 2) {  // Space for " (" and null terminator
            *os_info++ = ' ';
            *os_info++ = '(';
            remaining -= 2;
            size_t date_len = strlen(info.build_date);
            if (date_len < remaining - 1) {  // -1 for closing parenthesis
                strncpy(os_info, info.build_date, remaining - 1);
                os_info += date_len;
                *os_info++ = ')';
                *os_info = '\0';
            }
        }
    }

    // Copy CPU info with explicit null termination
    strncpy(cpu_info, info.cpu, cpu_size - 1);
    cpu_info[cpu_size - 1] = '\0';
}

// Get current git commit hash
void get_git_commit(char* commit_hash, size_t hash_size) {
    FILE* pipe = popen("git rev-parse HEAD", "r");
    if (pipe) {
        if (fgets(commit_hash, hash_size, pipe) != NULL) {
            commit_hash[strcspn(commit_hash, "\n")] = 0;  // Remove newline
        }
        pclose(pipe);
    } else {
        strncpy(commit_hash, "Unknown", hash_size - 1);
        commit_hash[hash_size - 1] = '\0';
    }
}

// Parse script parameters from comments
void parse_script_params(const char* content, bench_script_t* script) {
    script->iterations = DEFAULT_ITERATIONS;
    script->expected_time = 0.0;

    // Find the first line that starts with ;;
    const char* line = strstr(content, ";;");
    if (!line)
        return;

    // Create a copy of the line without the newline
    char line_copy[256];
    size_t i = 0;
    line += 2;  // Skip ;;

    // Skip whitespace
    while (*line == ' ' || *line == '\t')
        line++;

    // Copy until newline or end of string
    while (*line && *line != '\n' && i < sizeof(line_copy) - 1)
        line_copy[i++] = *line++;
    line_copy[i] = '\0';

    // Split the line into tokens and parse each parameter
    char* token = strtok(line_copy, " \t");
    while (token) {
        if (strncmp(token, "--iterations=", 12) == 0) {
            sscanf(token + 12, "%d", &script->iterations);
        } else if (strncmp(token, "--expected-time=", 16) == 0) {
            sscanf(token + 16, "%lf", &script->expected_time);
        }
        token = strtok(NULL, " \t");
    }
}

// Run benchmark and collect results
void run_benchmark(bench_script_t* script, bench_result_t* result) {
    strncpy(result->script_name, script->name, sizeof(result->script_name) - 1);
    result->script_name[sizeof(result->script_name) - 1] = '\0';
    result->expected_time = script->expected_time;

    // Get system info
    get_system_info(result->os_info, sizeof(result->os_info), result->cpu_info, sizeof(result->cpu_info));
    get_git_commit(result->git_commit, sizeof(result->git_commit));

    // Get current timestamp
    time_t now;
    time(&now);
    strftime(result->timestamp, sizeof(result->timestamp), "%Y-%m-%d %H:%M:%S", localtime(&now));

    // Run init script if exists
    if (script->init_script[0] != '\0') {
        runtime_create(0, NULL);
        eval_str(script->init_script);
        runtime_destroy();
    }

    // Run benchmark iterations
    double total_time = 0;
    result->min_time = 1e9;
    result->max_time = 0;

    int iterations = script->iterations > 0 ? script->iterations : 10;  // Default to 10 iterations

    for (int i = 0; i < iterations; i++) {
        runtime_create(0, NULL);
        struct timeval start, end;
        gettimeofday(&start, NULL);
        eval_str(script->content);
        gettimeofday(&end, NULL);
        runtime_destroy();

        double iteration_time = (end.tv_sec - start.tv_sec) * 1000.0;  // Convert to milliseconds
        iteration_time += (end.tv_usec - start.tv_usec) / 1000.0;      // Add microseconds part

        total_time += iteration_time;
        if (iteration_time < result->min_time)
            result->min_time = iteration_time;
        if (iteration_time > result->max_time)
            result->max_time = iteration_time;
    }

    result->avg_time = total_time / iterations;
}

// Load previous benchmark results
void load_previous_results(bench_results_t* results) {
    FILE* file = fopen(BENCH_RESULTS_FILE, "r");
    if (!file) {
        results->result_count = 0;
        return;
    }

    char line[1024];
    int current_result = -1;
    results->result_count = 0;

    while (fgets(line, sizeof(line), file)) {
        // Remove whitespace and newlines
        char* start = line;
        while (*start && (*start == ' ' || *start == '\t' || *start == '\n'))
            start++;
        if (!*start)
            continue;

        // Parse JSON-like format
        if (strstr(start, "\"script\":")) {
            current_result = results->result_count++;
            char* script_name = strchr(start, ':');
            if (script_name) {
                script_name = strchr(script_name, '"');
                if (script_name) {
                    script_name++;
                    char* end = strchr(script_name, '"');
                    if (end) {
                        size_t len = end - script_name;
                        strncpy(results->results[current_result].script_name, script_name, len);
                        results->results[current_result].script_name[len] = '\0';
                    }
                }
            }
        } else if (strstr(start, "\"min_time\":")) {
            results->results[current_result].min_time = atof(strchr(start, ':') + 1);
        } else if (strstr(start, "\"max_time\":")) {
            results->results[current_result].max_time = atof(strchr(start, ':') + 1);
        } else if (strstr(start, "\"avg_time\":")) {
            results->results[current_result].avg_time = atof(strchr(start, ':') + 1);
        } else if (strstr(start, "\"expected_time\":")) {
            results->results[current_result].expected_time = atof(strchr(start, ':') + 1);
        } else if (strstr(start, "\"timestamp\":")) {
            char* timestamp = strchr(start, ':');
            if (timestamp) {
                timestamp = strchr(timestamp, '"');
                if (timestamp) {
                    timestamp++;
                    char* end = strchr(timestamp, '"');
                    if (end) {
                        size_t len = end - timestamp;
                        strncpy(results->results[current_result].timestamp, timestamp, len);
                        results->results[current_result].timestamp[len] = '\0';
                    }
                }
            }
        } else if (strstr(start, "\"os_info\":")) {
            char* os_info = strchr(start, ':');
            if (os_info) {
                os_info = strchr(os_info, '"');
                if (os_info) {
                    os_info++;
                    char* end = strchr(os_info, '"');
                    if (end) {
                        size_t len = end - os_info;
                        strncpy(results->results[current_result].os_info, os_info, len);
                        results->results[current_result].os_info[len] = '\0';
                    }
                }
            }
        } else if (strstr(start, "\"cpu_info\":")) {
            char* cpu_info = strchr(start, ':');
            if (cpu_info) {
                cpu_info = strchr(cpu_info, '"');
                if (cpu_info) {
                    cpu_info++;
                    char* end = strchr(cpu_info, '"');
                    if (end) {
                        size_t len = end - cpu_info;
                        strncpy(results->results[current_result].cpu_info, cpu_info, len);
                        results->results[current_result].cpu_info[len] = '\0';
                    }
                }
            }
        } else if (strstr(start, "\"git_commit\":")) {
            char* git_commit = strchr(start, ':');
            if (git_commit) {
                git_commit = strchr(git_commit, '"');
                if (git_commit) {
                    git_commit++;
                    char* end = strchr(git_commit, '"');
                    if (end) {
                        size_t len = end - git_commit;
                        strncpy(results->results[current_result].git_commit, git_commit, len);
                        results->results[current_result].git_commit[len] = '\0';
                    }
                }
            }
        }
    }

    fclose(file);
}

// Save benchmark results
void save_results(bench_results_t* results) {
    FILE* file = fopen(BENCH_RESULTS_FILE, "w");
    if (!file) {
        printf("Error: Could not open results file for writing\n");
        return;
    }

    fprintf(file, "{\n");
    fprintf(file, "  \"results\": [\n");
    for (int i = 0; i < results->result_count; i++) {
        fprintf(file, "    {\n");
        fprintf(file, "      \"script\": \"%s\",\n", results->results[i].script_name);
        fprintf(file, "      \"min_time\": %.3f,\n", results->results[i].min_time);
        fprintf(file, "      \"max_time\": %.3f,\n", results->results[i].max_time);
        fprintf(file, "      \"avg_time\": %.3f,\n", results->results[i].avg_time);
        if (results->results[i].expected_time > 0) {
            fprintf(file, "      \"expected_time\": %.3f,\n", results->results[i].expected_time);
        }
        fprintf(file, "      \"timestamp\": \"%s\",\n", results->results[i].timestamp);
        fprintf(file, "      \"os_info\": \"%s\",\n", results->results[i].os_info);
        fprintf(file, "      \"cpu_info\": \"%s\",\n", results->results[i].cpu_info);
        fprintf(file, "      \"git_commit\": \"%s\"\n", results->results[i].git_commit);
        fprintf(file, "    }%s\n", i < results->result_count - 1 ? "," : "");
    }
    fprintf(file, "  ]\n");
    fprintf(file, "}\n");
    fclose(file);
}

// Compare and print results
void compare_and_print_results(bench_result_t* current, bench_result_t* previous) {
    printf("\n%sBenchmark Results for %s:%s\n", CYAN, current->script_name, RESET);
    printf("%s----------------------------------------%s\n", YELLOW, RESET);

    if (previous) {
        printf("%sPrevious Run:%s %s\n", BLUE, RESET, previous->timestamp);
        printf("%sCurrent Run:%s  %s\n\n", BLUE, RESET, current->timestamp);
    }

    printf("%sPerformance Metrics:%s\n", MAGENTA, RESET);
    if (previous) {
        printf("  %sMin Time:%s %.3f ms ", BLUE, RESET, current->min_time);
        print_colored_diff(current->min_time, previous->min_time);
        printf("\n");

        printf("  %sMax Time:%s %.3f ms ", BLUE, RESET, current->max_time);
        print_colored_diff(current->max_time, previous->max_time);
        printf("\n");

        printf("  %sAvg Time:%s %.3f ms ", BLUE, RESET, current->avg_time);
        print_colored_diff(current->avg_time, previous->avg_time);
        printf("\n");

        printf("  %sExp Time:%s %.3f ms ", BLUE, RESET, current->expected_time);
        print_colored_diff(current->avg_time, current->expected_time);
        printf("\n");
        printf("\n");

        // Print summary
        double avg_diff_percent = ((current->avg_time - previous->avg_time) / previous->avg_time) * 100;
        if (fabs(avg_diff_percent) > 5) {
            printf("\n%sSummary:%s Performance has ", MAGENTA, RESET);
            if (avg_diff_percent > 0) {
                printf("%sdegraded by %.1f%%%s", RED, avg_diff_percent, RESET);
            } else {
                printf("%simproved by %.1f%%%s", GREEN, -avg_diff_percent, RESET);
            }
            printf(" since last run\n");
        } else {
            printf("\n%sSummary:%s Performance is %sstable%s (%.1f%% change)\n", MAGENTA, RESET, YELLOW, RESET,
                   avg_diff_percent);
        }
    } else {
        printf("  %sMin Time:%s %.3f ms %s(new)%s\n", BLUE, RESET, current->min_time, GREEN, RESET);
        printf("  %sMax Time:%s %.3f ms %s(new)%s\n", BLUE, RESET, current->max_time, GREEN, RESET);
        printf("  %sAvg Time:%s %.3f ms %s(new)%s\n", BLUE, RESET, current->avg_time, GREEN, RESET);
        printf("  %sExp Time:%s %.3f ms\n", BLUE, RESET, current->expected_time);
        printf("\n%sSummary:%s First run of this benchmark\n", MAGENTA, RESET);
    }

    // Always show expected time comparison if it's set
    if (current->expected_time > 0.0) {
        printf("\n%sExpected Time:%s %.3f ms ", BLUE, RESET, current->expected_time);
        print_expected_time_diff(current->avg_time, current->expected_time);
        printf("\n");
    }

    printf("%s----------------------------------------%s\n\n", YELLOW, RESET);
}

// Print system information
void print_system_info(bench_result_t* result) {
    printf("\n%sSystem Information:%s\n", CYAN, RESET);
    printf("%s----------------------------------------%s\n", YELLOW, RESET);
    printf("  %sOS:%s %s\n", BLUE, RESET, result->os_info);
    printf("  %sCPU:%s %s\n", BLUE, RESET, result->cpu_info);
    printf("  %sGit Commit:%s %s\n", BLUE, RESET, result->git_commit);
    printf("  %sTimestamp:%s %s\n", BLUE, RESET, result->timestamp);
    printf("%s----------------------------------------%s\n\n", YELLOW, RESET);
}

// Print colored diff between current and previous results
void print_colored_diff(double current, double previous) {
    double diff = ((current - previous) / previous) * 100.0;
    if (diff > 0) {
        printf("%s+%.1f%%%s", RED, diff, RESET);
    } else if (diff < 0) {
        printf("%s%.1f%%%s", GREEN, diff, RESET);
    } else {
        printf("%s0.0%%%s", YELLOW, RESET);
    }
}

void print_expected_time_diff(double actual, double expected) {
    double diff = ((actual - expected) / expected) * 100.0;
    if (fabs(diff) < 5.0) {
        printf("%s(within ±5%% of expected)%s", YELLOW, RESET);
    } else if (diff > 0) {
        printf("%s(%.1f%% slower than expected)%s", RED, diff, RESET);
    } else {
        printf("%s(%.1f%% faster than expected)%s", GREEN, -diff, RESET);
    }
}

void process_script_file(const char* filename, bench_results_t* results) {
    char full_path[MAX_PATH_LEN];
    char init_path[MAX_PATH_LEN];
    bench_script_t script = {0};

    // Get script name without extension
    const char* base_name = strrchr(filename, '/');
    if (!base_name)
        base_name = filename;
    else
        base_name++;

    char* dot = strrchr(base_name, '.');
    if (dot) {
        size_t name_len = dot - base_name;
        if (name_len >= sizeof(script.name)) {
            name_len = sizeof(script.name) - 1;
        }
        strncpy(script.name, base_name, name_len);
        script.name[name_len] = '\0';
    } else {
        strncpy(script.name, base_name, sizeof(script.name) - 1);
        script.name[sizeof(script.name) - 1] = '\0';
    }

    // Read main script
    strncpy(full_path, filename, sizeof(full_path) - 1);
    full_path[sizeof(full_path) - 1] = '\0';

    FILE* file = fopen(full_path, "r");
    if (!file) {
        printf("Error: Could not open script file %s\n", full_path);
        return;
    }

    size_t content_size = fread(script.content, 1, sizeof(script.content) - 1, file);
    script.content[content_size] = '\0';
    fclose(file);

    // Try to read init script if it exists
    size_t written =
        snprintf(init_path, sizeof(init_path), "%s/%s%s.rf", BENCH_SCRIPTS_DIR, script.name, BENCH_INIT_SUFFIX);
    if (written >= sizeof(init_path)) {
        printf("Warning: Init script path truncated for %s\n", script.name);
    }

    file = fopen(init_path, "r");
    if (file) {
        size_t init_size = fread(script.init_script, 1, sizeof(script.init_script) - 1, file);
        script.init_script[init_size] = '\0';
        fclose(file);
    }

    // Parse script parameters
    parse_script_params(script.content, &script);

    // Run benchmark
    bench_result_t current_result;
    run_benchmark(&script, &current_result);

    // Find previous result for comparison
    bench_result_t* previous_result = NULL;
    for (int i = 0; i < results->result_count; i++) {
        if (strcmp(results->results[i].script_name, script.name) == 0) {
            previous_result = &results->results[i];
            break;
        }
    }

    compare_and_print_results(&current_result, previous_result);

    // Update results array
    if (results->result_count < MAX_RESULTS) {
        results->results[results->result_count++] = current_result;
    }
}

void scan_benchmark_scripts(bench_results_t* results) {
    char cmd[MAX_SCRIPT_NAME];
    snprintf(cmd, sizeof(cmd), "find %s -name '*.rf' -not -name '*%s.rf'", BENCH_SCRIPTS_DIR, BENCH_INIT_SUFFIX);

    FILE* pipe = popen(cmd, "r");
    if (!pipe) {
        printf("Error: Could not scan benchmark scripts directory\n");
        return;
    }

    // Get system info from the first script
    char filename[MAX_SCRIPT_NAME];
    if (fgets(filename, sizeof(filename), pipe) != NULL) {
        filename[strcspn(filename, "\n")] = 0;
        bench_script_t script = {0};
        strcpy(script.name, "system");
        run_benchmark(&script, &results->results[0]);
        print_system_info(&results->results[0]);
    }

    // Reset pipe to start from beginning
    pclose(pipe);
    pipe = popen(cmd, "r");

    while (fgets(filename, sizeof(filename), pipe) != NULL) {
        filename[strcspn(filename, "\n")] = 0;
        process_script_file(filename, results);
    }

    pclose(pipe);
}

int main() {
    bench_results_t results;
    load_previous_results(&results);

    // Scan and process all benchmark scripts
    scan_benchmark_scripts(&results);

    // Save updated results
    save_results(&results);

    return 0;
}