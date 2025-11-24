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

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include <sys/types.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <dirent.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <math.h>
#include <stdbool.h>
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

    // Run benchmark iterations
    double total_time = 0;
    result->min_time = 1e9;
    result->max_time = 0;

    int iterations = script->iterations > 0 ? script->iterations : 10;  // Default to 10 iterations

    // Initialize runtime environment once for all iterations
    runtime_create(0, NULL);

    // Run initialization script if present
    if (script->init_script[0] != '\0') {
        printf("Evaluating init script for %s (%zu bytes)\n", script->name, strlen(script->init_script));
        obj_p v = eval_str(script->init_script);
        if (IS_ERR(v)) {
            obj_p r = obj_fmt(v, 1);
            printf("Error in initialization script [%s]: %s\n", script->name, AS_C8(r));
            drop_obj(v);
            drop_obj(r);
            runtime_destroy();
            return;
        }
        drop_obj(v);
    }

    for (int i = 0; i < iterations; i++) {
        struct timeval start, end;
        gettimeofday(&start, NULL);
        obj_p v = eval_str(script->content);
        if (IS_ERR(v)) {
            obj_p r = obj_fmt(v, 1);
            printf("Error in benchmark script [%s]: %s\n", script->name, AS_C8(r));
            drop_obj(v);
            drop_obj(r);
            runtime_destroy();
            return;
        }

        drop_obj(v);
        gettimeofday(&end, NULL);

        double iteration_time = (end.tv_sec - start.tv_sec) * 1000.0;  // Convert to milliseconds
        iteration_time += (end.tv_usec - start.tv_usec) / 1000.0;      // Add microseconds part

        total_time += iteration_time;
        if (iteration_time < result->min_time)
            result->min_time = iteration_time;
        if (iteration_time > result->max_time)
            result->max_time = iteration_time;
    }

    result->avg_time = total_time / iterations;

    // Cleanup runtime environment
    runtime_destroy();
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
    // First load existing results
    bench_results_t existing_results = {0};
    load_previous_results(&existing_results);

    // Update only the results that were run
    for (int i = 0; i < results->result_count; i++) {
        bool found = false;
        for (int j = 0; j < existing_results.result_count; j++) {
            if (strcmp(results->results[i].script_name, existing_results.results[j].script_name) == 0) {
                // Update existing result
                existing_results.results[j] = results->results[i];
                found = true;
                break;
            }
        }
        if (!found && existing_results.result_count < MAX_RESULTS) {
            // Add new result if there's space
            existing_results.results[existing_results.result_count++] = results->results[i];
        }
    }

    // Save the combined results
    FILE* file = fopen(BENCH_RESULTS_FILE, "w");
    if (!file) {
        printf("Error: Could not open results file for writing\n");
        return;
    }

    fprintf(file, "{\n");
    fprintf(file, "  \"results\": [\n");
    for (int i = 0; i < existing_results.result_count; i++) {
        fprintf(file, "    {\n");
        fprintf(file, "      \"script\": \"%s\",\n", existing_results.results[i].script_name);
        fprintf(file, "      \"min_time\": %.3f,\n", existing_results.results[i].min_time);
        fprintf(file, "      \"max_time\": %.3f,\n", existing_results.results[i].max_time);
        fprintf(file, "      \"avg_time\": %.3f,\n", existing_results.results[i].avg_time);
        if (existing_results.results[i].expected_time > 0) {
            fprintf(file, "      \"expected_time\": %.3f,\n", existing_results.results[i].expected_time);
        }
        fprintf(file, "      \"timestamp\": \"%s\",\n", existing_results.results[i].timestamp);
        fprintf(file, "      \"os_info\": \"%s\",\n", existing_results.results[i].os_info);
        fprintf(file, "      \"cpu_info\": \"%s\",\n", existing_results.results[i].cpu_info);
        fprintf(file, "      \"git_commit\": \"%s\"\n", existing_results.results[i].git_commit);
        fprintf(file, "    }%s\n", i < existing_results.result_count - 1 ? "," : "");
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
    char script_path[MAX_PATH_LEN];
    char init_path[MAX_PATH_LEN];
    char script_name[MAX_SCRIPT_NAME] = {0};  // Initialize to zero

    // Extract script name without extension
    const char* base = strrchr(filename, '/');
    if (!base)
        base = filename;
    else
        base++;

    // Copy script name and remove .rfl extension if present
    size_t name_len = strlen(base);
    if (name_len >= sizeof(script_name)) {
        name_len = sizeof(script_name) - 1;
    }
    memcpy(script_name, base, name_len);
    script_name[name_len] = '\0';

    char* ext = strrchr(script_name, '.');
    if (ext && strcmp(ext, ".rfl") == 0) {
        *ext = '\0';
        name_len = ext - script_name;  // Update name_len after removing extension
    }

    // Construct paths with bounds checking
    size_t written = snprintf(script_path, sizeof(script_path), "%s/%s.rfl", BENCH_SCRIPTS_DIR, script_name);
    if (written >= sizeof(script_path)) {
        printf("Error: Script path too long for %s\n", script_name);
        return;
    }
    written = snprintf(init_path, sizeof(init_path), "%s/%s%s.rfl", BENCH_SCRIPTS_DIR, script_name, BENCH_INIT_SUFFIX);
    if (written >= sizeof(init_path)) {
        printf("Error: Init path too long for %s\n", script_name);
        return;
    }

    // Read script content
    FILE* script_file = fopen(script_path, "r");
    if (!script_file) {
        printf("Error: Could not open script file %s\n", script_path);
        return;
    }

    bench_script_t script = {0};  // Initialize entire struct to zero

    // Copy script name - we already know it fits since we used the same buffer size
    memcpy(script.name, script_name, name_len);
    script.name[name_len] = '\0';

    // Read script content with bounds checking
    size_t content_size = fread(script.content, 1, sizeof(script.content) - 1, script_file);
    script.content[content_size] = '\0';
    fclose(script_file);

    // Read init script if exists
    FILE* init_file = fopen(init_path, "r");
    if (init_file) {
        size_t init_size = fread(script.init_script, 1, sizeof(script.init_script) - 1, init_file);
        script.init_script[init_size] = '\0';
        fclose(init_file);
    } else {
        // Clear the init script buffer explicitly to ensure it's empty
        script.init_script[0] = '\0';
    }

    // Parse script parameters
    parse_script_params(script.content, &script);

    // Run benchmark and store results if there's space
    if (results->result_count < MAX_RESULTS) {
        bench_result_t result = {0};  // Initialize to zero
        run_benchmark(&script, &result);
        results->results[results->result_count++] = result;
    } else {
        printf("Warning: Maximum number of results reached, skipping %s\n", script_name);
    }
}

void scan_benchmark_scripts(bench_results_t* results) {
    char cmd[MAX_SCRIPT_NAME];
    snprintf(cmd, sizeof(cmd), "find %s -name '*.rfl' -not -name '*%s.rfl'", BENCH_SCRIPTS_DIR, BENCH_INIT_SUFFIX);

    FILE* pipe = popen(cmd, "r");
    if (!pipe) {
        printf("Error: Could not scan benchmark scripts directory\n");
        return;
    }

    // Initialize result count - we'll start with actual benchmark scripts
    results->result_count = 0;

    // Get system info and print it directly
    get_system_info(results->results[0].os_info, sizeof(results->results[0].os_info), results->results[0].cpu_info,
                    sizeof(results->results[0].cpu_info));
    get_git_commit(results->results[0].git_commit, sizeof(results->results[0].git_commit));

    // Get current timestamp
    time_t now;
    time(&now);
    strftime(results->results[0].timestamp, sizeof(results->results[0].timestamp), "%Y-%m-%d %H:%M:%S",
             localtime(&now));

    // Print system info without running a benchmark
    print_system_info(&results->results[0]);

    while (fgets(cmd, sizeof(cmd), pipe) != NULL) {
        cmd[strcspn(cmd, "\n")] = 0;
        process_script_file(cmd, results);
    }

    pclose(pipe);
}

int main(int argc, char* argv[]) {
    bench_results_t results = {0};  // Initialize entire struct to zero
    bench_results_t previous_results = {0};
    bool specific_tests = false;
    bool has_errors = false;

    // Load previous results first
    load_previous_results(&previous_results);

    // Check if specific tests were requested via command line or BENCH env var
    if (argc > 1) {
        specific_tests = true;
        // Process each test name provided
        for (int i = 1; i < argc; i++) {
            const char* test_name = argv[i];
            char script_path[MAX_PATH_LEN];

            // Check if .rfl extension is already present
            const char* ext = strstr(test_name, ".rfl");
            if (ext) {
                snprintf(script_path, sizeof(script_path), "%s/%s", BENCH_SCRIPTS_DIR, test_name);
            } else {
                snprintf(script_path, sizeof(script_path), "%s/%s.rfl", BENCH_SCRIPTS_DIR, test_name);
            }

            process_script_file(script_path, &results);
        }
    } else {
        // Check BENCH environment variable
        const char* bench_var = getenv("BENCH");
        if (bench_var && *bench_var) {
            specific_tests = true;
            char script_path[MAX_PATH_LEN];
            snprintf(script_path, sizeof(script_path), "%s/%s.rfl", BENCH_SCRIPTS_DIR, bench_var);
            process_script_file(script_path, &results);
        } else {
            // If no BENCH variable, scan all benchmark scripts
            scan_benchmark_scripts(&results);
        }
    }

    // Update only the results that were run
    if (specific_tests) {
        // For each result we just ran
        for (int i = 1; i < results.result_count; i++) {
            // Find matching previous result
            bench_result_t* previous = NULL;
            for (int j = 0; j < previous_results.result_count; j++) {
                if (strcmp(results.results[i].script_name, previous_results.results[j].script_name) == 0) {
                    previous = &previous_results.results[j];
                    break;
                }
            }
            // Compare and print results for this test
            compare_and_print_results(&results.results[i], previous);
        }
    } else {
        // Compare and print results for each test
        for (int i = 0; i < results.result_count; i++) {
            bench_result_t* previous = NULL;
            for (int j = 0; j < previous_results.result_count; j++) {
                if (strcmp(results.results[i].script_name, previous_results.results[j].script_name) == 0) {
                    previous = &previous_results.results[j];
                    break;
                }
            }
            compare_and_print_results(&results.results[i], previous);
        }
    }

    // Save results if we have any
    if (results.result_count > 0) {
        // If running specific tests, only update those tests in the previous results
        if (specific_tests) {
            for (int i = 1; i < results.result_count; i++) {
                for (int j = 0; j < previous_results.result_count; j++) {
                    if (strcmp(results.results[i].script_name, previous_results.results[j].script_name) == 0) {
                        previous_results.results[j] = results.results[i];
                        break;
                    }
                }
            }
            save_results(&previous_results);
        } else {
            save_results(&results);
        }
    }

    return has_errors ? 1 : 0;
}