#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <unistd.h>
#include "odu.h"

static void usage(void) {
    fprintf(stderr, "Usage: odu [options] -- <program> [<arg>...]\n");
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -cd DIR               Working directory for spawned process\n");
    fprintf(stderr, "  -stderr MODE          How to handle stderr (console|disable|redirect_to_stdout)\n");
    fprintf(stderr, "  -log FILE             Enable logging to file (or |1 for stdout, |2 for stderr)\n");
    fprintf(stderr, "  -protocol_version V   Protocol version (must be 1.0)\n");
    fprintf(stderr, "  -v                    Print version and exit\n");
}

// Custom argument parsing to handle single-dash long options like Go's flag package
static int parse_args(int argc, char *argv[], config_t *config, int *version_flag) {
    int i = 1;

    while (i < argc) {
        if (strcmp(argv[i], "--") == 0) {
            // Skip the separator and return next index
            return i + 1;
        } else if (strcmp(argv[i], "-v") == 0) {
            *version_flag = 1;
            i++;
        } else if (strcmp(argv[i], "-cd") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Option -cd requires an argument\n");
                return -1;
            }
            config->working_dir = argv[i + 1];
            i += 2;
        } else if (strcmp(argv[i], "-stderr") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Option -stderr requires an argument\n");
                return -1;
            }
            config->stderr_mode = argv[i + 1];
            i += 2;
        } else if (strcmp(argv[i], "-log") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Option -log requires an argument\n");
                return -1;
            }
            config->log_file = argv[i + 1];
            i += 2;
        } else if (strcmp(argv[i], "-protocol_version") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Option -protocol_version requires an argument\n");
                return -1;
            }
            config->protocol_version = argv[i + 1];
            i += 2;
        } else {
            // All remaining args are the command
            break;
        }
    }

    return i; // Return index of first non-option argument
}

int main(int argc, char *argv[]) {
    config_t config = {
        .working_dir = ".",
        .stderr_mode = "console",
        .log_file = NULL,
        .protocol_version = NULL,
        .args = NULL,
        .argc = 0
    };

    int version_flag = 0;
    int cmd_start = parse_args(argc, argv, &config, &version_flag);

    if (cmd_start < 0) {
        usage();
        return 3;
    }

    init_logger(config.log_file);

    if (version_flag) {
        printf("odu version: %s\nprotocol_version: %s\n", VERSION, PROTOCOL_VERSION);
        close_logger();
        return 0;
    }

    // Validate stderr mode
    if (strcmp(config.stderr_mode, "console") != 0 &&
        strcmp(config.stderr_mode, "disable") != 0 &&
        strcmp(config.stderr_mode, "redirect_to_stdout") != 0) {
        log_printf("invalid stderr flag: %s\n", config.stderr_mode);
        close_logger();
        return 3;
    }

    // Get remaining arguments (the command to execute)
    if (cmd_start >= argc) {
        fprintf(stderr, "Not enough arguments.\n");
        usage();
        close_logger();
        return 3;
    }

    // Validate protocol version
    if (config.protocol_version == NULL || strcmp(config.protocol_version, "1.0") != 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Invalid version specified: %s  Supported version: %s",
                 config.protocol_version ? config.protocol_version : "(null)", PROTOCOL_VERSION);

        uint8_t error_msg[256];
        int error_len = snprintf((char *)error_msg, sizeof(error_msg), "%s", msg);

        // Write error packet directly (before process loop starts)
        protocol_write_buf_t write_buf;
        init_protocol_write_buf(&write_buf);
        queue_write_packet(&write_buf, START_ERROR, error_msg, error_len);
        while (write_buf.written < write_buf.data_len) {
            try_write_to_fd(STDOUT_FILENO, &write_buf);
        }

        fprintf(stderr, "%s\n", msg);
        usage();
        close_logger();
        return 3;
    }

    config.args = &argv[cmd_start];
    config.argc = argc - cmd_start;

    log_printf("dir:%s, log:%s, protocol_version:%s, stderr:%s\n",
               config.working_dir, config.log_file ? config.log_file : "(null)",
               config.protocol_version, config.stderr_mode);

    int result = execute_process(&config);

    close_logger();
    return result;
}
