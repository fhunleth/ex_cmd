#ifndef ODU_H
#define ODU_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

// Protocol constants
#define SEND_INPUT 1
#define SEND_OUTPUT 2
#define OUTPUT 3
#define INPUT 4
#define CLOSE_INPUT 5
#define OUTPUT_EOF 6
#define COMMAND_ENV 7
#define PID 8
#define START_ERROR 9
#define CLOSE_OUTPUT 10
#define KILL 11
#define EXIT_STATUS 12

// Buffer size for I/O operations
#define BUFFER_SIZE ((1 << 16) - 5)
#define PROTOCOL_BUFFER_SIZE (1 << 17)  // 128KB for protocol I/O buffering

// Version information
#define VERSION "0.1.0"
#define PROTOCOL_VERSION "1.0"

// Packet structure
typedef struct {
    uint8_t tag;
    uint32_t data_len;
    uint8_t *data;
} packet_t;

// Protocol I/O buffers
typedef struct {
    uint8_t buffer[PROTOCOL_BUFFER_SIZE];
    size_t data_len;     // Amount of valid data in buffer
    size_t consumed;     // Amount of data already processed
} protocol_read_buf_t;

typedef struct {
    uint8_t buffer[PROTOCOL_BUFFER_SIZE];
    size_t data_len;     // Amount of data to write
    size_t written;      // Amount already written
} protocol_write_buf_t;

// Configuration
typedef struct {
    char *working_dir;
    char *stderr_mode;
    char *log_file;
    char *protocol_version;
    char **args;
    int argc;
} config_t;

// Logging
extern FILE *log_fp;
void init_logger(const char *log_flag);
void log_printf(const char *format, ...);
void close_logger(void);

// Protocol functions
void init_protocol_read_buf(protocol_read_buf_t *buf);
void init_protocol_write_buf(protocol_write_buf_t *buf);
int try_read_from_fd(int fd, protocol_read_buf_t *buf);
int try_write_to_fd(int fd, protocol_write_buf_t *buf);
int try_read_packet(protocol_read_buf_t *buf, packet_t *packet);
int queue_write_packet(protocol_write_buf_t *buf, uint8_t tag, const uint8_t *data, uint32_t data_len);
void free_packet(packet_t *packet);

// Process execution
int execute_process(config_t *config);

// Utility functions
void die(const char *msg);
void fatal(const char *msg);

#endif // ODU_H
