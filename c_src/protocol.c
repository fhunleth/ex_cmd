#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <errno.h>
#include "odu.h"

void init_protocol_read_buf(protocol_read_buf_t *buf) {
    buf->data_len = 0;
    buf->consumed = 0;
}

void init_protocol_write_buf(protocol_write_buf_t *buf) {
    buf->data_len = 0;
    buf->written = 0;
}

// Try to read more data from fd into buffer
// Returns: 0 on success, -1 on EOF, -2 on error
int try_read_from_fd(int fd, protocol_read_buf_t *buf) {
    // Compact buffer if needed
    if (buf->consumed > 0) {
        size_t remaining = buf->data_len - buf->consumed;
        if (remaining > 0) {
            memmove(buf->buffer, buf->buffer + buf->consumed, remaining);
        }
        buf->data_len = remaining;
        buf->consumed = 0;
    }

    // Read as much as we can
    size_t available_space = PROTOCOL_BUFFER_SIZE - buf->data_len;
    if (available_space == 0) {
        // Buffer full - can't read more
        return 0;
    }

    ssize_t n = read(fd, buf->buffer + buf->data_len, available_space);
    if (n > 0) {
        buf->data_len += n;
        return 0;
    } else if (n == 0) {
        return -1;  // EOF
    } else {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            fatal("FATAL: EAGAIN/EWOULDBLOCK in try_read_from_fd - should not happen with blocking I/O! This indicates a double-read bug.");
        }
        return -2;  // Real error
    }
}

// Try to write buffered data to fd
// Returns: 0 on success, -1 on error
int try_write_to_fd(int fd, protocol_write_buf_t *buf) {
    if (buf->written >= buf->data_len) {
        // Nothing to write
        return 0;
    }

    size_t remaining = buf->data_len - buf->written;
    ssize_t n = write(fd, buf->buffer + buf->written, remaining);

    if (n > 0) {
        buf->written += n;

        // If everything is written, reset buffer
        if (buf->written >= buf->data_len) {
            buf->data_len = 0;
            buf->written = 0;
        }
        return 0;
    } else if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // Would block - this is expected with non-blocking I/O
            return 0;
        }
        if (errno == EPIPE) {
            return -1;  // Broken pipe
        }
        return -1;  // Error
    }

    return 0;
}

// Try to parse a complete packet from the read buffer
// Returns: 0 if packet read, -1 if incomplete (need more data), -2 on error
int try_read_packet(protocol_read_buf_t *buf, packet_t *packet) {
    size_t available = buf->data_len - buf->consumed;

    // Need at least 5 bytes (4 for length + 1 for tag)
    if (available < 5) {
        return -1;  // Need more data
    }

    // Read length
    uint32_t length;
    memcpy(&length, buf->buffer + buf->consumed, sizeof(uint32_t));
    length = ntohl(length);

    if (length < 1 || length > BUFFER_SIZE + 1) {
        log_printf("Invalid packet length: %u\n", length);
        return -2;  // Error
    }

    // Check if we have the complete packet
    uint32_t packet_size = sizeof(uint32_t) + length;  // 4-byte length + tag + data
    if (available < packet_size) {
        return -1;  // Need more data
    }

    // We have a complete packet - parse it
    uint8_t tag = buf->buffer[buf->consumed + sizeof(uint32_t)];
    uint32_t data_len = length - 1;

    packet->tag = tag;
    packet->data_len = data_len;

    if (data_len > 0) {
        packet->data = malloc(data_len);
        if (packet->data == NULL) {
            fatal("Failed to allocate packet data");
        }
        memcpy(packet->data, buf->buffer + buf->consumed + sizeof(uint32_t) + 1, data_len);
    } else {
        packet->data = NULL;
    }

    // Mark this packet as consumed
    buf->consumed += packet_size;

    return 0;
}

// Queue a packet to be written
// Returns: 0 on success, -1 if buffer is full
int queue_write_packet(protocol_write_buf_t *buf, uint8_t tag, const uint8_t *data, uint32_t data_len) {
    uint32_t payload_len = data_len + 1;  // tag + data
    uint32_t packet_size = sizeof(uint32_t) + payload_len;  // length + tag + data

    // Check if we have space
    size_t available = PROTOCOL_BUFFER_SIZE - buf->data_len;
    if (available < packet_size) {
        log_printf("Write buffer full, cannot queue packet\n");
        return -1;
    }

    // Write length
    uint32_t network_len = htonl(payload_len);
    memcpy(buf->buffer + buf->data_len, &network_len, sizeof(uint32_t));
    buf->data_len += sizeof(uint32_t);

    // Write tag
    buf->buffer[buf->data_len] = tag;
    buf->data_len += 1;

    // Write data
    if (data_len > 0 && data != NULL) {
        memcpy(buf->buffer + buf->data_len, data, data_len);
        buf->data_len += data_len;
    }

    return 0;
}

void free_packet(packet_t *packet) {
    if (packet->data != NULL) {
        free(packet->data);
        packet->data = NULL;
    }
    packet->data_len = 0;
}
