#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include "odu.h"

FILE *log_fp = NULL;

void init_logger(const char *log_flag) {
    if (log_flag == NULL || strlen(log_flag) == 0) {
        log_fp = NULL;
        return;
    }

    if (strcmp(log_flag, "|1") == 0) {
        log_fp = stdout;
    } else if (strcmp(log_flag, "|2") == 0) {
        log_fp = stderr;
    } else {
        log_fp = fopen(log_flag, "a");
        if (log_fp == NULL) {
            fprintf(stderr, "[odu]: Failed to open log file: %s\n", log_flag);
        }
    }
}

void log_printf(const char *format, ...) {
    if (log_fp == NULL) {
        return;
    }

    va_list args;
    va_start(args, format);
    fprintf(log_fp, "[odu]: ");
    vfprintf(log_fp, format, args);
    fflush(log_fp);
    va_end(args);
}

void close_logger(void) {
    if (log_fp != NULL && log_fp != stdout && log_fp != stderr) {
        fclose(log_fp);
        log_fp = NULL;
    }
}

void die(const char *msg) {
    log_printf("dying: %s\n", msg);
    fprintf(stderr, "%s\n", msg);
    close_logger();
    exit(-1);
}

void fatal(const char *msg) {
    log_printf("fatal: %s\n", msg);
    fprintf(stderr, "%s\n", msg);
    close_logger();
    exit(-1);
}
