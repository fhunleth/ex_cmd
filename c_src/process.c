#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <errno.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include "odu.h"

typedef struct {
    pid_t child_pid;
    int stdin_pipe[2];
    int stdout_pipe[2];
    int stderr_pipe[2];
    volatile sig_atomic_t should_exit;
    volatile sig_atomic_t should_kill;
    config_t *config;
} process_state_t;

static process_state_t *global_state = NULL;

static void signal_handler(int sig) {
    if (global_state != NULL) {
        log_printf("Received signal: %d\n", sig);
        global_state->should_exit = 1;
    }
}

static void setup_signal_handlers(void) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);
}

static char **read_env_from_stdin(protocol_read_buf_t *read_buf) {
    packet_t packet;

    // Read from stdin until we have a complete packet
    while (true) {
        int read_result = try_read_from_fd(STDIN_FILENO, read_buf);
        if (read_result == -1 || read_result == -2) {
            fatal("Failed to read environment packet");
        }

        int packet_result = try_read_packet(read_buf, &packet);
        if (packet_result == 0) {
            // Got a packet
            break;
        } else if (packet_result == -2) {
            fatal("Invalid environment packet");
        }
        // -1 means need more data, keep reading
    }

    if (packet.tag != COMMAND_ENV) {
        fatal("First packet must be command environment");
    }

    // Parse environment variables
    int env_count = 0;
    int max_env = 100;
    char **env = malloc(sizeof(char *) * (max_env + 1));
    if (env == NULL) {
        fatal("Failed to allocate environment array");
    }

    uint32_t i = 0;
    while (i < packet.data_len) {
        if (i + 2 > packet.data_len) break;

        uint16_t entry_len = (packet.data[i] << 8) | packet.data[i + 1];
        i += 2;

        if (i + entry_len > packet.data_len) break;

        if (env_count >= max_env) {
            max_env *= 2;
            env = realloc(env, sizeof(char *) * (max_env + 1));
            if (env == NULL) {
                fatal("Failed to reallocate environment array");
            }
        }

        env[env_count] = malloc(entry_len + 1);
        if (env[env_count] == NULL) {
            fatal("Failed to allocate environment entry");
        }

        memcpy(env[env_count], &packet.data[i], entry_len);
        env[env_count][entry_len] = '\0';
        env_count++;

        i += entry_len;
    }

    env[env_count] = NULL;

    free_packet(&packet);

    log_printf("Read %d environment variables\n", env_count);

    return env;
}

static void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        fatal("Failed to get file flags");
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        fatal("Failed to set non-blocking");
    }
}

static int spawn_child(process_state_t *state, char **env) {
    if (pipe(state->stdin_pipe) != 0) {
        fatal("Failed to create stdin pipe");
    }

    if (pipe(state->stdout_pipe) != 0) {
        fatal("Failed to create stdout pipe");
    }

    pid_t pid = fork();

    if (pid < 0) {
        fatal("Failed to fork");
    }

    if (pid == 0) {
        // Child process
        close(state->stdin_pipe[1]);  // Close write end
        close(state->stdout_pipe[0]); // Close read end

        // Redirect stdin/stdout
        dup2(state->stdin_pipe[0], STDIN_FILENO);
        dup2(state->stdout_pipe[1], STDOUT_FILENO);

        // Handle stderr based on config
        if (strcmp(state->config->stderr_mode, "disable") == 0) {
            int dev_null = open("/dev/null", O_WRONLY);
            if (dev_null != -1) {
                dup2(dev_null, STDERR_FILENO);
                close(dev_null);
            }
        } else if (strcmp(state->config->stderr_mode, "redirect_to_stdout") == 0) {
            dup2(STDOUT_FILENO, STDERR_FILENO);
        }
        // For "console", stderr remains as is

        close(state->stdin_pipe[0]);
        close(state->stdout_pipe[1]);

        // Change working directory
        if (chdir(state->config->working_dir) != 0) {
            fprintf(stderr, "Failed to change directory to %s\n", state->config->working_dir);
            exit(3);
        }

        // Set environment variables
        if (env != NULL) {
            // Combine parent environment with new environment
            // New environment variables override parent ones
            extern char **environ;

            int new_env_count = 0;
            while (env[new_env_count] != NULL) {
                new_env_count++;
            }

            // Count non-overridden parent variables
            int parent_env_count = 0;
            while (environ[parent_env_count] != NULL) {
                parent_env_count++;
            }

            char **combined_env = malloc(sizeof(char *) * (parent_env_count + new_env_count + 1));
            if (combined_env == NULL) {
                exit(3);
            }

            int combined_idx = 0;

            // First, add all new environment variables
            for (int i = 0; i < new_env_count; i++) {
                combined_env[combined_idx++] = env[i];
            }

            // Then add parent environment variables that aren't being overridden
            for (int i = 0; i < parent_env_count; i++) {
                char *parent_var = environ[i];
                char *equals = strchr(parent_var, '=');
                if (equals == NULL) continue;

                size_t key_len = equals - parent_var;
                int is_overridden = 0;

                // Check if this variable is overridden in new env
                for (int j = 0; j < new_env_count; j++) {
                    if (strncmp(parent_var, env[j], key_len) == 0 && env[j][key_len] == '=') {
                        is_overridden = 1;
                        break;
                    }
                }

                if (!is_overridden) {
                    combined_env[combined_idx++] = parent_var;
                }
            }

            combined_env[combined_idx] = NULL;

            environ = combined_env;
        }

        // Execute command
        execvp(state->config->args[0], state->config->args);

        // If we reach here, exec failed
        fprintf(stderr, "Failed to execute: %s\n", state->config->args[0]);
        exit(3);
    }

    // Parent process
    close(state->stdin_pipe[0]);  // Close read end
    close(state->stdout_pipe[1]); // Close write end

    state->child_pid = pid;

    // Set non-blocking mode for macOS compatibility
    // This is only needed on the write end of stdin pipe
    set_nonblocking(state->stdin_pipe[1]);

    return 0;
}

static void write_pid_packet(protocol_write_buf_t *write_buf, pid_t pid) {
    uint32_t pid_val = htonl((uint32_t)pid);
    queue_write_packet(write_buf, PID, (uint8_t *)&pid_val, sizeof(uint32_t));

    // Send initial SEND_INPUT to signal we're ready for input
    queue_write_packet(write_buf, SEND_INPUT, NULL, 0);
}

static void write_exit_status(protocol_write_buf_t *write_buf, int status) {
    uint32_t exit_code = htonl((uint32_t)status);
    log_printf("Command exited with status: %d\n", status);
    queue_write_packet(write_buf, EXIT_STATUS, (uint8_t *)&exit_code, sizeof(uint32_t));
}

static void process_stdin_packet(process_state_t *state, packet_t *packet,
                                  protocol_write_buf_t *proto_write_buf,
                                  bool *stdin_closed, bool *stdout_closed,
                                  bool *output_requested, uint32_t *requested_chunk_size,
                                  bool *close_stdin_requested,
                                  uint8_t **stdin_write_buffer, size_t *stdin_write_remaining,
                                  size_t *stdin_write_offset) {
    switch (packet->tag) {
        case KILL:
            log_printf("=== Received KILL command ===\n");
            log_printf("child_pid: %d\n", state->child_pid);
            state->should_kill = 1;
            state->should_exit = 1;
            break;

        case CLOSE_INPUT:
            if (!*stdin_closed) {
                // Set flag to close stdin after buffer drains
                *close_stdin_requested = true;
                log_printf("CLOSE_INPUT received (buffer: %zu bytes)\n", *stdin_write_remaining);
            }
            break;

        case INPUT:
            if (*stdin_closed) {
                fatal("Trying to send input on closed stdin");
            }

            if (packet->data_len > 0) {
                // If we already have pending data, just append to buffer
                if (*stdin_write_remaining > 0) {
                    // Realloc to fit new data
                    size_t new_size = *stdin_write_remaining + packet->data_len;
                    uint8_t *new_buffer = realloc(*stdin_write_buffer, new_size);
                    if (new_buffer == NULL) {
                        fatal("Failed to reallocate stdin write buffer");
                    }
                    memcpy(new_buffer + *stdin_write_remaining, packet->data, packet->data_len);
                    *stdin_write_buffer = new_buffer;
                    *stdin_write_remaining = new_size;
                } else {
                    // No pending data - buffer it for the select loop to write
                    *stdin_write_remaining = packet->data_len;
                    *stdin_write_buffer = malloc(*stdin_write_remaining);
                    if (*stdin_write_buffer == NULL) {
                        fatal("Failed to allocate stdin write buffer");
                    }
                    memcpy(*stdin_write_buffer, packet->data, *stdin_write_remaining);
                    *stdin_write_offset = 0;
                }
                // Don't send SEND_INPUT yet - will send when buffer is drained
            } else {
                // Empty packet, just send SEND_INPUT
                queue_write_packet(proto_write_buf, SEND_INPUT, NULL, 0);
            }
            break;

        case SEND_OUTPUT: {
            if (*stdout_closed) {
                log_printf("Request for output on closed stdout\n");
                break;
            }

            if (packet->data_len != 4) {
                fatal("Invalid SEND_OUTPUT packet size");
            }

            uint32_t chunk_size = ntohl(*(uint32_t *)packet->data);
            if (chunk_size > BUFFER_SIZE) {
                chunk_size = BUFFER_SIZE;
            }

            // Store the request - we'll read when stdout becomes readable
            *output_requested = true;
            *requested_chunk_size = chunk_size;
            log_printf("Output requested, chunk_size: %u\n", chunk_size);
            break;
        }

        case CLOSE_OUTPUT:
            if (!*stdout_closed) {
                log_printf("Closing stdout\n");
                close(state->stdout_pipe[0]);
                queue_write_packet(proto_write_buf, OUTPUT_EOF, NULL, 0);
                *stdout_closed = true;
            }
            break;

        case SEND_INPUT:
            // This shouldn't normally be sent by Elixir to us
            log_printf("Unexpected SEND_INPUT from Elixir\n");
            break;

        default:
            log_printf("Unknown packet tag: %d\n", packet->tag);
            break;
    }
}

int execute_process(config_t *config) {
    process_state_t state = {
        .child_pid = -1,
        .should_exit = 0,
        .should_kill = 0,
        .config = config
    };

    global_state = &state;
    setup_signal_handlers();

    // Initialize protocol I/O buffers
    protocol_read_buf_t proto_read_buf;
    protocol_write_buf_t proto_write_buf;
    init_protocol_read_buf(&proto_read_buf);
    init_protocol_write_buf(&proto_write_buf);

    char **env = read_env_from_stdin(&proto_read_buf);

    if (spawn_child(&state, env) != 0) {
        return 3;
    }

    write_pid_packet(&proto_write_buf, state.child_pid);

    bool stdin_closed = false;
    bool stdout_closed = false;
    bool child_exited = false;
    int child_exit_code = 0;
    bool output_requested = false;  // Track if Elixir requested output
    uint32_t requested_chunk_size = 0;
    bool close_stdin_requested = false;  // Track if CLOSE_INPUT was received

    // Buffer for pending stdin write
    uint8_t *stdin_write_buffer = NULL;
    size_t stdin_write_remaining = 0;
    size_t stdin_write_offset = 0;

    packet_t packet;

    // Main event loop
    // Continue until:
    // 1. We're explicitly told to exit (should_exit flag from KILL or protocol stdin EOF)
    // 2. OR child has exited and stdout is closed (normal termination)
    while (!state.should_exit && !(child_exited && stdout_closed)) {
        fd_set readfds, writefds;
        FD_ZERO(&readfds);
        FD_ZERO(&writefds);
        FD_SET(STDIN_FILENO, &readfds);

        int max_fd = STDIN_FILENO;

        // Monitor protocol stdout for writability if we have pending data
        if (proto_write_buf.written < proto_write_buf.data_len) {
            FD_SET(STDOUT_FILENO, &writefds);
            if (STDOUT_FILENO > max_fd) {
                max_fd = STDOUT_FILENO;
            }
        }

        // Monitor stdin pipe for writability if we have pending data
        if (stdin_write_remaining > 0 && !stdin_closed) {
            FD_SET(state.stdin_pipe[1], &writefds);
            if (state.stdin_pipe[1] > max_fd) {
                max_fd = state.stdin_pipe[1];
            }
        }

        // Only monitor stdout if we have a pending output request
        if (output_requested && !stdout_closed) {
            FD_SET(state.stdout_pipe[0], &readfds);
            if (state.stdout_pipe[0] > max_fd) {
                max_fd = state.stdout_pipe[0];
            }
        }

        struct timeval tv = {.tv_sec = 0, .tv_usec = 10000}; // 10ms timeout

        int ret = select(max_fd + 1, &readfds, &writefds, NULL, &tv);

        // Handle protocol stdout writability
        if (ret > 0 && FD_ISSET(STDOUT_FILENO, &writefds)) {
            try_write_to_fd(STDOUT_FILENO, &proto_write_buf);
        }

        // Handle stdin pipe writability
        if (ret > 0 && stdin_write_remaining > 0 && FD_ISSET(state.stdin_pipe[1], &writefds)) {
            ssize_t written = write(state.stdin_pipe[1],
                                   stdin_write_buffer + stdin_write_offset,
                                   stdin_write_remaining);
            if (written > 0) {
                stdin_write_offset += written;
                stdin_write_remaining -= written;

                if (stdin_write_remaining == 0) {
                    // All data written, send SEND_INPUT and free buffer
                    free(stdin_write_buffer);
                    stdin_write_buffer = NULL;
                    stdin_write_offset = 0;
                    queue_write_packet(&proto_write_buf, SEND_INPUT, NULL, 0);

                    // If close was requested and buffer is now empty, close stdin
                    if (close_stdin_requested && !stdin_closed) {
                        log_printf("Buffer drained, closing stdin as requested\n");
                        close(state.stdin_pipe[1]);
                        stdin_closed = true;
                    }
                }
            } else if (written < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    fatal("FATAL: EAGAIN/EWOULDBLOCK writing to child stdin - should not happen with blocking I/O! This indicates a double-write bug.");
                }
                if (errno != EPIPE && errno != EBADF) {
                    log_printf("Error writing to child stdin: %s\n", strerror(errno));
                }
                // Clear the buffer on error
                free(stdin_write_buffer);
                stdin_write_buffer = NULL;
                stdin_write_remaining = 0;
                stdin_write_offset = 0;
            }
        }

        // Check if stdin close was requested and there's no buffered data
        if (close_stdin_requested && !stdin_closed && stdin_write_remaining == 0) {
            log_printf("Closing stdin as requested (no buffered data)\n");
            close(state.stdin_pipe[1]);
            stdin_closed = true;
        }

        // Handle stdout being readable
        if (ret > 0 && output_requested && FD_ISSET(state.stdout_pipe[0], &readfds)) {
            uint8_t buffer[BUFFER_SIZE];
            ssize_t bytes_read = read(state.stdout_pipe[0], buffer, requested_chunk_size);

            if (bytes_read > 0) {
                queue_write_packet(&proto_write_buf, OUTPUT, buffer, bytes_read);
                output_requested = false;  // Request fulfilled
            } else if (bytes_read == 0 || (bytes_read < 0 && (errno == EPIPE || errno == EBADF))) {
                log_printf("Child stdout closed\n");
                queue_write_packet(&proto_write_buf, OUTPUT_EOF, NULL, 0);
                stdout_closed = true;
                output_requested = false;
            } else if (bytes_read < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    fatal("FATAL: EAGAIN/EWOULDBLOCK reading from child stdout - should not happen with blocking I/O! This indicates a double-read bug.");
                }
                log_printf("Error reading from child stdout: %s\n", strerror(errno));
            }
        }

        // Handle protocol stdin readability
        if (ret > 0 && FD_ISSET(STDIN_FILENO, &readfds)) {
            int read_result = try_read_from_fd(STDIN_FILENO, &proto_read_buf);
            if (read_result == -1) {
                log_printf("stdin closed\n");
                state.should_exit = 1;
                break;
            } else if (read_result == -2) {
                log_printf("stdin read error\n");
                state.should_exit = 1;
                break;
            }

            // Try to parse packets
            while (true) {
                int packet_result = try_read_packet(&proto_read_buf, &packet);
                if (packet_result == 0) {
                    // Got a packet, process it
                    process_stdin_packet(&state, &packet, &proto_write_buf,
                                       &stdin_closed, &stdout_closed,
                                       &output_requested, &requested_chunk_size,
                                       &close_stdin_requested,
                                       &stdin_write_buffer, &stdin_write_remaining, &stdin_write_offset);
                    free_packet(&packet);

                    // Check if we should exit after processing packet
                    if (state.should_exit) {
                        log_printf("Breaking loop due to should_exit flag\n");
                        break;
                    }
                } else if (packet_result == -2) {
                    log_printf("Invalid packet\n");
                    state.should_exit = 1;
                    break;
                } else {
                    // -1: need more data
                    break;
                }
            }

            if (state.should_exit) {
                break;
            }
        } else if (ret < 0 && errno != EINTR) {
            log_printf("select error: %s\n", strerror(errno));
            break;
        }

        // Check if child has exited (but don't exit immediately - allow draining output)
        if (!child_exited) {
            int status;
            pid_t result = waitpid(state.child_pid, &status, WNOHANG);
            if (result > 0) {
                child_exited = true;
                if (WIFEXITED(status)) {
                    child_exit_code = WEXITSTATUS(status);
                } else if (WIFSIGNALED(status)) {
                    // Match Go's ExitCode() behavior: return -1 for signal-based termination
                    child_exit_code = -1;
                }
                log_printf("Child exited with code: %d\n", child_exit_code);

                // Close stdin to child since it's dead
                if (!stdin_closed) {
                    close(state.stdin_pipe[1]);
                    stdin_closed = true;
                }
            }
        }
    }

    // Close stdin to child if not already closed
    // This allows the child process to exit gracefully (e.g., cat exits on stdin EOF)
    if (!stdin_closed) {
        log_printf("Closing stdin to child\n");
        close(state.stdin_pipe[1]);
        stdin_closed = true;
    }

    // Send final EOF if not already sent and we're not in kill mode
    // In kill mode, we want to send EXIT_STATUS as quickly as possible
    if (!stdout_closed && !state.should_kill) {
        queue_write_packet(&proto_write_buf, OUTPUT_EOF, NULL, 0);
        close(state.stdout_pipe[0]);
        stdout_closed = true;
    }

    // If child hasn't exited yet, give it time to exit gracefully
    // especially after stdin was closed (many programs exit when stdin closes)
    // Skip grace period if we're in kill mode
    if (!child_exited && !state.should_kill) {
        log_printf("Child has not exited yet, waiting for graceful exit\n");
        int status;
        // Wait up to 100ms for child to exit on its own
        for (int i = 0; i < 2 && !child_exited; i++) {
            pid_t result = waitpid(state.child_pid, &status, WNOHANG);
            log_printf("Grace period waitpid iteration %d, result: %d\n", i, result);
            if (result > 0) {
                if (WIFEXITED(status)) {
                    child_exit_code = WEXITSTATUS(status);
                } else if (WIFSIGNALED(status)) {
                    // Match Go's ExitCode() behavior: return -1 for signal-based termination
                    child_exit_code = -1;
                }
                child_exited = true;
                log_printf("Child exited gracefully with code: %d\n", child_exit_code);
                break;
            }
            usleep(100000); // 100ms
        }
    }

    // If child still hasn't exited, we need to terminate it
    if (!child_exited) {
        if (state.should_kill) {
            log_printf("Killing child process %d immediately\n", state.child_pid);
            int kill_result = kill(state.child_pid, SIGKILL);
            if (kill_result != 0) {
                log_printf("kill() failed: %s\n", strerror(errno));
            }
        } else {
            log_printf("Sending SIGTERM to child\n");
            kill(state.child_pid, SIGTERM);

            // Wait up to 200ms for graceful exit after SIGTERM
            int status;
            for (int i = 0; i < 4; i++) {
                pid_t result = waitpid(state.child_pid, &status, WNOHANG);
                if (result > 0) {
                    if (WIFEXITED(status)) {
                        child_exit_code = WEXITSTATUS(status);
                    } else if (WIFSIGNALED(status)) {
                        // Match Go's ExitCode() behavior: return -1 for signal-based termination
                        child_exit_code = -1;
                    }
                    child_exited = true;
                    break;
                }
                usleep(50000); // 50ms
            }

            if (!child_exited) {
                log_printf("Child did not exit gracefully, killing child %d\n", state.child_pid);
                int kill_result = kill(state.child_pid, SIGKILL);
                if (kill_result != 0) {
                    log_printf("kill() failed: %s\n", strerror(errno));
                }
            }
        }

        // Final wait if still not exited
        // Use WNOHANG in a loop to avoid blocking forever
        // But keep it short since Elixir side has limited patience
        if (!child_exited) {
            log_printf("Waiting for child to exit after SIGKILL\n");
            int status;
            // Wait up to 20ms for child to exit after SIGKILL (4 iterations * 5ms)
            for (int i = 0; i < 4; i++) {
                pid_t result = waitpid(state.child_pid, &status, WNOHANG);
                log_printf("waitpid returned: %d (iteration %d)\n", result, i);
                if (result > 0) {
                    if (WIFEXITED(status)) {
                        child_exit_code = WEXITSTATUS(status);
                        log_printf("Child exited normally with code: %d\n", child_exit_code);
                    } else if (WIFSIGNALED(status)) {
                        // Match Go's ExitCode() behavior: return -1 for signal-based termination
                        child_exit_code = -1;
                        log_printf("Child killed by signal, exit code: -1\n");
                    }
                    child_exited = true;
                    break;
                } else if (result < 0) {
                    log_printf("waitpid failed: %s\n", strerror(errno));
                    break;
                }
                usleep(5000); // 5ms
            }

            if (!child_exited) {
                log_printf("WARNING: Child did not exit after SIGKILL within 50ms\n");
                // Exit anyway - the process is orphaned or in a bad state
                child_exit_code = -1;
            }
        }
    }

    log_printf("About to send EXIT_STATUS: %d\n", child_exit_code);
    write_exit_status(&proto_write_buf, child_exit_code);
    log_printf("EXIT_STATUS sent successfully\n");

    // Flush any remaining protocol output
    while (proto_write_buf.written < proto_write_buf.data_len) {
        fd_set writefds;
        FD_ZERO(&writefds);
        FD_SET(STDOUT_FILENO, &writefds);

        struct timeval tv = {.tv_sec = 1, .tv_usec = 0};  // 1 second timeout
        int ret = select(STDOUT_FILENO + 1, NULL, &writefds, NULL, &tv);

        if (ret > 0 && FD_ISSET(STDOUT_FILENO, &writefds)) {
            if (try_write_to_fd(STDOUT_FILENO, &proto_write_buf) < 0) {
                break;
            }
        } else if (ret < 0) {
            log_printf("select error during flush: %s\n", strerror(errno));
            break;
        } else {
            // Timeout
            log_printf("Timeout flushing output\n");
            break;
        }
    }

    // Free stdin write buffer if any
    if (stdin_write_buffer != NULL) {
        free(stdin_write_buffer);
    }

    // Free environment
    if (env != NULL) {
        for (int i = 0; env[i] != NULL; i++) {
            free(env[i]);
        }
        free(env);
    }

    return 0;
}
