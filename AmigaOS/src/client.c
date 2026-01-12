#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <proto/exec.h>
#include <proto/bsdsocket.h>
#include <proto/dos.h>

// ignore that STRPTR is unsigned char *, but all string literals are char *
#pragma GCC diagnostic ignored "-Wpointer-sign"

#define PORT 10203
#define BUFFER_SIZE 8192
#define MESSAGE "Hello, Server!"
#define EXPECTED_RESPONSE "Hello, Client!"

struct RDArgs *rdargs;
struct Library *SocketBase;

int main(int argc, char *argv[])
{
    struct sockaddr_in server_addr;
    int res;
    char *server_ip;

    // Define the template for ReadArgs
    const char *template = "SERVERIP/A";
    LONG args[1];

    // Initialize the RDArgs structure
    rdargs = ReadArgs(template, args, NULL);
    if (!rdargs) {
        printf("Usage: %s SERVERIP\n", argv[0]);
        return 1;
    }

    server_ip = (char *)args[0];

    // Open the bsdsocket.library
    SocketBase = OpenLibrary("bsdsocket.library", 4);
    if (!SocketBase) {
        printf("Failed to open bsdsocket.library\n");
        FreeArgs(rdargs);
        return 1;
    }

    int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        PrintFault(IoErr(), "socket");
        CloseLibrary(SocketBase);
        FreeArgs(rdargs);
        return 1;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = inet_addr(server_ip);

    printf("Connecting to %s...\n", server_ip);
    res = connect(socket_fd, (struct sockaddr *)&server_addr, sizeof(server_addr));
    if (res < 0) {
        PrintFault(IoErr(), "connect");
        CloseSocket(socket_fd);
        CloseLibrary(SocketBase);
        FreeArgs(rdargs);
        return 1;
    }
    printf("Connected!\n");

    // Send initial handshake
    ssize_t bytes_written = send(socket_fd, (char *)MESSAGE, strlen(MESSAGE), 0);
    if (bytes_written < 0) {
        PrintFault(IoErr(), "send");
        CloseSocket(socket_fd);
        CloseLibrary(SocketBase);
        FreeArgs(rdargs);
        return 1;
    }

    // Receive handshake response
    char buffer[BUFFER_SIZE];
    ssize_t bytes_read = recv(socket_fd, buffer, BUFFER_SIZE - 1, 0);
    if (bytes_read < 0) {
        PrintFault(IoErr(), "recv");
        CloseSocket(socket_fd);
        CloseLibrary(SocketBase);
        FreeArgs(rdargs);
        return 1;
    }

    buffer[bytes_read] = '\0';
    printf("Received handshake: %s\n", buffer);

    if (strcmp(buffer, EXPECTED_RESPONSE) != 0) {
        printf("Unexpected handshake response.\n");
        CloseSocket(socket_fd);
        CloseLibrary(SocketBase);
        FreeArgs(rdargs);
        return 1;
    }

    printf("Handshake complete. Receiving test packets...\n\n");

    // Receive and verify test packets
    int packet_num = 0;
    int errors = 0;

    while (1) {
        // Read length prefix (4 bytes)
        uint32_t net_size;
        size_t total_read = 0;
        while (total_read < sizeof(net_size)) {
            bytes_read = recv(socket_fd, ((char*)&net_size) + total_read,
                            sizeof(net_size) - total_read, 0);
            if (bytes_read <= 0) {
                if (bytes_read == 0) {
                    if (total_read == 0) {
                        printf("Connection closed by server.\n");
                        goto cleanup;
                    }
                    printf("Connection closed mid-length!\n");
                } else {
                    PrintFault(IoErr(), "recv length");
                }
                goto cleanup;
            }
            total_read += bytes_read;
        }

        size_t expected_size = ntohl(net_size);
        if (expected_size > BUFFER_SIZE - 1) {
            printf("Packet too large: %zu bytes\n", expected_size);
            errors++;
            break;
        }

        // Read the complete packet data (loop until we get all bytes)
        total_read = 0;
        while (total_read < expected_size) {
            bytes_read = recv(socket_fd, buffer + total_read,
                            expected_size - total_read, 0);
            if (bytes_read <= 0) {
                if (bytes_read == 0) {
                    printf("Connection closed mid-packet!\n");
                } else {
                    PrintFault(IoErr(), "recv data");
                }
                goto cleanup;
            }
            total_read += bytes_read;
        }

        packet_num++;
        buffer[total_read] = '\0';

        // Parse size from header to verify
        size_t header_size = 0;
        if (sscanf(buffer, "SIZE:%zu:", &header_size) == 1) {
            printf("Packet %d: Received %zu bytes (expected %zu)\n",
                   packet_num, total_read, header_size);

            // Verify size matches (both the length prefix and the header)
            if (total_read != expected_size || expected_size != header_size) {
                printf("  ERROR: Size mismatch! (length=%zu, total=%zu, header=%zu)\n",
                       expected_size, total_read, header_size);
                errors++;
            } else {
                // Verify pattern in payload
                size_t header_len = 0;
                for (size_t i = 0; i < total_read && buffer[i] != '\0'; i++) {
                    if (i > 0 && buffer[i-1] == ':') {
                        header_len = i;
                        break;
                    }
                }
                if (header_len == 0) header_len = strlen(buffer);

                // Check pattern consistency (note: pattern will be 0x00 if all NUL bytes)
                int pattern_ok = 1;
                if (header_len < total_read) {
                    unsigned char expected_pattern = buffer[header_len];
                    for (size_t i = header_len + 1; i < total_read; i++) {
                        if ((unsigned char)buffer[i] != expected_pattern) {
                            pattern_ok = 0;
                            break;
                        }
                    }
                }

                if (pattern_ok) {
                    printf("  OK: Pattern verified (0x%02X)\n",
                           header_len < total_read ? (unsigned char)buffer[header_len] : 0);
                } else {
                    printf("  ERROR: Pattern mismatch!\n");
                    errors++;
                }
            }
        } else {
            printf("Packet %d: Received %zu bytes (unknown format)\n",
                   packet_num, total_read);
        }

        // Send acknowledgment
        char ack[32];
        snprintf(ack, sizeof(ack), "ACK:%d", packet_num);
        bytes_written = send(socket_fd, ack, strlen(ack), 0);
        if (bytes_written < 0) {
            PrintFault(IoErr(), "send");
            break;
        }
    }

cleanup:
    printf("\n");
    printf("RX test complete: %d packets received, %d errors\n", packet_num, errors);

    // Define test sizes for TX (same as server)
    const size_t tx_test_sizes[] = {
        64, 100, 256, 512, 1024, 1518, 2048, 4096
    };
    size_t tx_num_tests = sizeof(tx_test_sizes) / sizeof(tx_test_sizes[0]);

    printf("\nNow sending test packets to server...\n\n");

    int tx_errors = 0;
    for (size_t i = 0; i < tx_num_tests; i++) {
        size_t size = tx_test_sizes[i];

        // Create test buffer
        char *test_buffer = malloc(size);
        if (!test_buffer) {
            printf("malloc failed\n");
            tx_errors++;
            break;
        }

        // Fill with pattern
        snprintf(test_buffer, size, "SIZE:%zu:", size);
        size_t header_len = strlen(test_buffer);
        unsigned char pattern = 0x51 + (i % 26);  // 'Q' to 'Z' range
        for (size_t j = header_len; j < size; j++) {
            test_buffer[j] = pattern;
        }

        printf("TX Packet %zu/%zu: Sending %zu bytes (pattern: 0x%02X)...\n",
               i + 1, tx_num_tests, size, pattern);

        // Send length prefix
        uint32_t net_size = htonl((uint32_t)size);
        bytes_written = send(socket_fd, (char*)&net_size, sizeof(net_size), 0);
        if (bytes_written < 0) {
            PrintFault(IoErr(), "send length");
            free(test_buffer);
            tx_errors++;
            break;
        }

        // Send data
        size_t total_sent = 0;
        while (total_sent < size) {
            bytes_written = send(socket_fd, test_buffer + total_sent,
                               size - total_sent, 0);
            if (bytes_written < 0) {
                PrintFault(IoErr(), "send data");
                tx_errors++;
                break;
            }
            total_sent += bytes_written;
        }

        free(test_buffer);

        if (total_sent != size) {
            printf("  ERROR: Only sent %zu of %zu bytes\n", total_sent, size);
            tx_errors++;
            continue;
        }

        // Wait for acknowledgment
        bytes_read = recv(socket_fd, buffer, BUFFER_SIZE - 1, 0);
        if (bytes_read < 0) {
            PrintFault(IoErr(), "recv ack");
            tx_errors++;
            break;
        }
        buffer[bytes_read] = '\0';
        printf("  Server ACK: %s\n\n", buffer);
    }

    printf("\n");
    printf("TX test complete: %zu packets sent, %d errors\n", tx_num_tests, tx_errors);
    printf("\n");
    printf("Overall: RX=%d errors, TX=%d errors\n", errors, tx_errors);

    CloseSocket(socket_fd);
    CloseLibrary(SocketBase);
    FreeArgs(rdargs);

    return (errors > 0 || tx_errors > 0) ? 1 : 0;
}