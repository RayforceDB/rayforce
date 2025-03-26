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
#define _XOPEN_SOURCE
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
    char timestamp[64];
    char os_info[256];
    char cpu_info[256];
    char git_commit[64];
} bench_result_t;

typedef struct {
    bench_result_t results[MAX_RESULTS];
    int result_count;
} bench_results_t;

// Function declarations
void get_system_info(char* os_info, char* cpu_info);
void get_git_commit(char* commit_hash);
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
void get_system_info(char* os_info, char* cpu_info) {
    struct utsname uname_data;
    if (uname(&uname_data) == 0) {
        snprintf(os_info, 256, "%s %s %s", uname_data.sysname, uname_data.release, uname_data.machine);
    }

    // On macOS, we can use sysctl to get CPU info
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "sysctl -n machdep.cpu.brand_string");
    FILE* pipe = popen(cmd, "r");
    if (pipe) {
        if (fgets(cpu_info, 256, pipe) != NULL) {
            cpu_info[strcspn(cpu_info, "\n")] = 0;  // Remove newline
        }
        pclose(pipe);
    } else {
        strcpy(cpu_info, "Unknown CPU");
    }
}

// Get current git commit hash
void get_git_commit(char* commit_hash) {
    FILE* pipe = popen("git rev-parse HEAD", "r");
    if (pipe) {
        if (fgets(commit_hash, 64, pipe) != NULL) {
            commit_hash[strcspn(commit_hash, "\n")] = 0;  // Remove newline
        }
        pclose(pipe);
    } else {
        strcpy(commit_hash, "Unknown");
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
    strncpy(result->script_name, script->name, MAX_SCRIPT_NAME - 1);
    result->expected_time = script->expected_time;

    // Get system info
    get_system_info(result->os_info, result->cpu_info);
    get_git_commit(result->git_commit);

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

// Print colored diff between current and previous results
void print_colored_diff(double current, double previous) {
    double diff = ((current - previous) / previous) * 100.0;
    if (diff > 0) {
        printf("\033[31m+%.1f%%\033[0m", diff);
    } else if (diff < 0) {
        printf("\033[32m%.1f%%\033[0m", diff);
    } else {
        printf("0.0%%");
    }
}

void print_expected_time_diff(double actual, double expected) {
    double diff = ((actual - expected) / expected) * 100.0;
    if (fabs(diff) < 5.0) {
        printf("\033[33m(within ±5%% of expected)\033[0m");
    } else if (diff > 0) {
        printf("\033[31m(%.1f%% slower than expected)\033[0m", diff);
    } else {
        printf("\033[32m(%.1f%% faster than expected)\033[0m", -diff);
    }
}

// Compare and print results
void compare_and_print_results(bench_result_t* current, bench_result_t* previous) {
    printf("\nBenchmark Results for %s:\n", current->script_name);
    printf("----------------------------------------\n");

    if (previous) {
        printf("Previous Run: %s\n", previous->timestamp);
        printf("Current Run:  %s\n\n", current->timestamp);
    }

    printf("Performance Metrics:\n");
    if (previous) {
        printf("  Min Time: %.3f ms %+.1f%%\n", current->min_time,
               ((current->min_time - previous->min_time) / previous->min_time) * 100);
        printf("  Max Time: %.3f ms %+.1f%%\n", current->max_time,
               ((current->max_time - previous->max_time) / previous->max_time) * 100);
        printf("  Avg Time: %.3f ms %+.1f%%\n", current->avg_time,
               ((current->avg_time - previous->avg_time) / previous->avg_time) * 100);
        printf("  Exp Time: %.3f ms %+.1f%%\n", current->expected_time,
               ((current->avg_time - current->expected_time) / current->avg_time) * 100);
        printf("\n");

        // Print summary
        double avg_diff_percent = ((current->avg_time - previous->avg_time) / previous->avg_time) * 100;
        if (fabs(avg_diff_percent) > 5) {
            printf("\nSummary: Performance has ");
            if (avg_diff_percent > 0) {
                printf("\033[31mdegraded by %.1f%%\033[0m", avg_diff_percent);
            } else {
                printf("\033[32mimproved by %.1f%%\033[0m", -avg_diff_percent);
            }
            printf(" since last run\n");
        } else {
            printf("\nSummary: Performance is \033[33mstable\033[0m (%.1f%% change)\n", avg_diff_percent);
        }
    } else {
        printf("  Min Time: %.3f ms \033[32m(new)\033[0m\n", current->min_time);
        printf("  Max Time: %.3f ms \033[32m(new)\033[0m\n", current->max_time);
        printf("  Avg Time: %.3f ms \033[32m(new)\033[0m\n", current->avg_time);
        printf("  Exp Time: %.3f ms\n", current->expected_time);
        printf("\nSummary: First run of this benchmark\n");
    }

    // Always show expected time comparison if it's set
    if (current->expected_time > 0.0) {
        printf("\nExpected Time: %.3f ms ", current->expected_time);
        print_expected_time_diff(current->avg_time, current->expected_time);
        printf("\n");
    }

    printf("----------------------------------------\n\n");
}

// Print system information
void print_system_info(bench_result_t* result) {
    printf("\nSystem Information:\n");
    printf("----------------------------------------\n");
    printf("  OS: %s\n", result->os_info);
    printf("  CPU: %s\n", result->cpu_info);
    printf("  Git Commit: %s\n", result->git_commit);
    printf("  Timestamp: %s\n", result->timestamp);
    printf("----------------------------------------\n\n");
}

void process_script_file(const char* filename, bench_results_t* results) {
    char full_path[MAX_SCRIPT_NAME];
    char init_path[MAX_SCRIPT_NAME];
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
        strncpy(script.name, base_name, name_len);
        script.name[name_len] = '\0';
    } else {
        strcpy(script.name, base_name);
    }

    // Read main script
    snprintf(full_path, sizeof(full_path), "%s", filename);
    FILE* file = fopen(full_path, "r");
    if (!file) {
        printf("Error: Could not open script file %s\n", full_path);
        return;
    }

    size_t content_size = fread(script.content, 1, sizeof(script.content) - 1, file);
    script.content[content_size] = '\0';
    fclose(file);

    // Try to read init script if it exists
    snprintf(init_path, sizeof(init_path), "%s/%s%s.rf", BENCH_SCRIPTS_DIR, script.name, BENCH_INIT_SUFFIX);
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