/*
 * client - Amiga side of the openpci-rtl8029.device network test
 *
 * Copyright (c) 2025-2026 Steen Lund
 * SPDX-License-Identifier: MIT (see licenses/MIT.txt)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/time.h>
#include <proto/exec.h>
#include <proto/bsdsocket.h>
#include <proto/dos.h>

// ignore that STRPTR is unsigned char *, but all string literals are char *
#pragma GCC diagnostic ignored "-Wpointer-sign"

#define PORT 10203
#define BUFFER_SIZE 8192

// Odd and even sizes are both needed: the NE2000 moves data in 16-bit words,
// so odd-length frames take a different path in the driver.
// Must match test_sizes in the server.
static const size_t tx_test_sizes[] = {
    64, 65, 100, 101, 256, 333, 512, 1001, 1024, 1459, 1461, 1518, 2048, 4096
};
#define NUM_TEST_SIZES (sizeof(tx_test_sizes) / sizeof(tx_test_sizes[0]))

// UDP echo test: one datagram is exactly one Ethernet frame of payload + 42 bytes
// (14 Ethernet + 20 IP + 8 UDP), so unlike TCP - which retransmits and may quietly
// switch to smaller segments - this pins down the frame size on the wire. 18 is the
// 60-byte minimum frame, 1472 the 1514-byte maximum; every size near the top is covered.
static const size_t udp_test_sizes[] = {
    18, 19, 100, 101, 1000, 1400, 1464, 1465, 1466, 1467, 1468, 1469, 1470, 1471, 1472
};
#define NUM_UDP_SIZES (sizeof(udp_test_sizes) / sizeof(udp_test_sizes[0]))
#define UDP_HEADERS 42
#define UDP_ATTEMPTS 3
#define UDP_TIMEOUT_SECS 2
#define UDP_DONE "UDP-DONE"

// Optional throughput benchmark (BENCH switch): bulk TCP data in each direction with no
// disk involved. The client asks for it instead of sending UDP_DONE.
#define UDP_BENCH "UDP-BENCH"
#define BENCH_DONE "BENCH-DONE"
#define BENCH_BYTES (4UL * 1024 * 1024)   // per direction; must match the server

#define MESSAGE "Hello, Server!"
#define EXPECTED_RESPONSE "Hello, Client!"

struct RDArgs *rdargs;
struct Library *SocketBase;

int main(void)
{
    struct sockaddr_in server_addr;
    int res;
    char *server_ip;

    // Define the template for ReadArgs
    const char *template = "SERVERIP/A,BENCH/S";
    LONG args[2] = { 0, 0 };

    // Initialize the RDArgs structure
    rdargs = ReadArgs(template, args, NULL);
    if (!rdargs) {
        printf("Usage: client SERVERIP [BENCH]\n");
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
    /* On the heap: too big for the stack */
    char *buffer = malloc(BUFFER_SIZE);
    if (!buffer) {
        printf("Out of memory\n");
        CloseSocket(socket_fd);
        CloseLibrary(SocketBase);
        FreeArgs(rdargs);
        return 1;
    }
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
    int udp_errors = 0;
    const int expected_packets = NUM_TEST_SIZES;  // Server sends the same set of sizes

    while (packet_num < expected_packets) {
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
                // Find the end of "SIZE:NNN:" header (look for second colon)
                size_t header_len = 0;
                int colon_count = 0;
                for (size_t i = 0; i < total_read; i++) {
                    if (buffer[i] == ':') {
                        colon_count++;
                        if (colon_count == 2) {
                            header_len = i + 1;  // Position after second colon
                            break;
                        }
                    }
                }
                if (header_len == 0) header_len = strlen(buffer);

                // Check pattern consistency (note: pattern will be 0x00 if all NUL bytes)
                int pattern_ok = 1;
                if (header_len < total_read) {
                    unsigned char expected_pattern = buffer[header_len];
                    for (size_t i = header_len + 1; i < total_read; i++) {
                        if ((unsigned char)buffer[i] != (unsigned char)(expected_pattern + ((i - header_len) % 251))) {
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

    size_t tx_num_tests = NUM_TEST_SIZES;

    printf("\nNow sending test packets to server...\n\n");

    int tx_errors = 0;
    for (size_t i = 0; i < tx_num_tests; i++) {
        size_t size = tx_test_sizes[i];

        // Create test buffer
        printf("TX Packet %zu/%zu: Allocating %zu bytes...\n", i + 1, tx_num_tests, size);
        char *test_buffer = malloc(size);
        if (!test_buffer) {
            printf("  ERROR: malloc(%zu) failed\n", size);
            tx_errors++;
            break;
        }

        // Fill with pattern
        snprintf(test_buffer, size, "SIZE:%zu:", size);
        size_t header_len = strlen(test_buffer);
        unsigned char pattern = 0x51 + (i % 26);  // 'Q' to 'Z' range
        for (size_t j = header_len; j < size; j++) {
            test_buffer[j] = (char)(pattern + ((j - header_len) % 251));
        }

        printf("  Sending %zu bytes (pattern: 0x%02X)...\n", size, pattern);

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
                free(test_buffer);
                tx_errors++;
                goto tx_done;
            }
            if (bytes_written == 0) {
                printf("  ERROR: send returned 0\n");
                free(test_buffer);
                tx_errors++;
                goto tx_done;
            }
            total_sent += bytes_written;
        }

        free(test_buffer);
        test_buffer = NULL;

        if (total_sent != size) {
            printf("  ERROR: Only sent %zu of %zu bytes\n", total_sent, size);
            tx_errors++;
            goto tx_done;
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

tx_done:
    printf("\n");
    printf("TX test complete: %zu packets sent, %d errors\n", tx_num_tests, tx_errors);

    // Endianness test - send structured binary data
    printf("\nEndianness test: Sending binary data...\n");

    struct __attribute__((packed)) {
        uint32_t magic;      // 0xDEADBEEF
        uint16_t version;    // 0x1234
        uint32_t counter;    // 12345678
        float value;         // 3.14159
        uint64_t timestamp;  // 0x0123456789ABCDEF
    } test_data;

    printf("  Size of test_data: %zu bytes\n", sizeof(test_data));

    test_data.magic = htonl(0xDEADBEEF);
    test_data.version = htons(0x1234);
    test_data.counter = htonl(12345678);
    // Float requires special handling
    union { float f; uint32_t i; } float_conv;
    float_conv.f = 3.14159f;
    float_conv.i = htonl(float_conv.i);
    test_data.value = float_conv.f;
    // 64-bit value
    test_data.timestamp = ((uint64_t)htonl(0x01234567) << 32) | htonl(0x89ABCDEF);

    // Send length prefix
    uint32_t net_size = htonl((uint32_t)sizeof(test_data));
    bytes_written = send(socket_fd, (char*)&net_size, sizeof(net_size), 0);
    if (bytes_written < 0) {
        PrintFault(IoErr(), "send endian test length");
        tx_errors++;
    } else {
        // Send data
        bytes_written = send(socket_fd, (char*)&test_data, sizeof(test_data), 0);
        if (bytes_written < 0) {
            PrintFault(IoErr(), "send endian test data");
            tx_errors++;
        } else {
            printf("  Sent %d bytes of binary data\n", (int)bytes_written);

            // Wait for verification response
            bytes_read = recv(socket_fd, buffer, BUFFER_SIZE - 1, 0);
            if (bytes_read > 0) {
                buffer[bytes_read] = '\0';
                printf("  Server response: %s\n", buffer);
            }
        }
    }

    // Receive endianness test data from server
    printf("\nEndianness test (Server->Client): Receiving binary data...\n");

    // Read length prefix
    uint32_t srv_net_size;
    size_t srv_total_read = 0;
    while (srv_total_read < sizeof(srv_net_size)) {
        bytes_read = recv(socket_fd, ((char*)&srv_net_size) + srv_total_read,
                        sizeof(srv_net_size) - srv_total_read, 0);
        if (bytes_read <= 0) {
            printf("Failed to read server endian test length\n");
            goto final_cleanup;
        }
        srv_total_read += bytes_read;
    }

    size_t srv_expected_size = ntohl(srv_net_size);
    printf("  Expecting %zu bytes of binary data\n", srv_expected_size);

    // Read data
    char srv_binary_buffer[256];
    srv_total_read = 0;
    while (srv_total_read < srv_expected_size) {
        bytes_read = recv(socket_fd, srv_binary_buffer + srv_total_read,
                        srv_expected_size - srv_total_read, 0);
        if (bytes_read <= 0) {
            printf("Failed to read server endian test data\n");
            goto final_cleanup;
        }
        srv_total_read += bytes_read;
    }

    printf("  Received %zu bytes\n", srv_total_read);
    printf("  Raw bytes: ");
    for (size_t i = 0; i < srv_total_read && i < 32; i++) {
        printf("%02X ", (unsigned char)srv_binary_buffer[i]);
    }
    printf("\n");

    // Verify the data
    struct __attribute__((packed)) {
        uint32_t magic;
        uint16_t version;
        uint32_t counter;
        float value;
        uint64_t timestamp;
    } *srv_test_data = (void*)srv_binary_buffer;

    uint32_t srv_magic = ntohl(srv_test_data->magic);
    uint16_t srv_version = ntohs(srv_test_data->version);
    uint32_t srv_counter = ntohl(srv_test_data->counter);

    // Float conversion
    union { float f; uint32_t i; } srv_float_conv;
    srv_float_conv.f = srv_test_data->value;
    srv_float_conv.i = ntohl(srv_float_conv.i);
    float srv_value = srv_float_conv.f;

    // 64-bit conversion - read as two 32-bit values
    uint32_t srv_ts_high, srv_ts_low;
    memcpy(&srv_ts_high, &srv_binary_buffer[14], 4);
    memcpy(&srv_ts_low, &srv_binary_buffer[18], 4);
    srv_ts_high = ntohl(srv_ts_high);
    srv_ts_low = ntohl(srv_ts_low);
    uint64_t srv_timestamp = ((uint64_t)srv_ts_high << 32) | srv_ts_low;

    int srv_endian_errors = 0;
    printf("  Verifying received data:\n");
    printf("    magic:     0x%08lX (expected 0xCAFEBABE) %s\n", (unsigned long)srv_magic,
           srv_magic == 0xCAFEBABE ? "OK" : "ERROR");
    if (srv_magic != 0xCAFEBABE) srv_endian_errors++;

    printf("    version:   0x%04X (expected 0x5678) %s\n", srv_version,
           srv_version == 0x5678 ? "OK" : "ERROR");
    if (srv_version != 0x5678) srv_endian_errors++;

    printf("    counter:   %lu (expected 87654321) %s\n", (unsigned long)srv_counter,
           srv_counter == 87654321 ? "OK" : "ERROR");
    if (srv_counter != 87654321) srv_endian_errors++;

    printf("    value:     %.5f (expected 2.71828) %s\n", srv_value,
           (srv_value > 2.71827 && srv_value < 2.71829) ? "OK" : "ERROR");
    if (!(srv_value > 2.71827 && srv_value < 2.71829)) srv_endian_errors++;

    printf("    timestamp: 0x%08lX%08lX (expected 0xFEDCBA9876543210) %s\n",
           (unsigned long)(srv_timestamp >> 32), (unsigned long)(srv_timestamp & 0xFFFFFFFF),
           srv_timestamp == 0xFEDCBA9876543210ULL ? "OK" : "ERROR");
    if (srv_timestamp != 0xFEDCBA9876543210ULL) srv_endian_errors++;

    // Send response
    char srv_response[128];
    if (srv_endian_errors == 0) {
        snprintf(srv_response, sizeof(srv_response), "ENDIAN-OK: All values correct!");
    } else {
        snprintf(srv_response, sizeof(srv_response), "ENDIAN-ERROR: %d values incorrect", srv_endian_errors);
    }
    bytes_written = send(socket_fd, srv_response, strlen(srv_response), 0);
    if (bytes_written < 0) {
        PrintFault(IoErr(), "send endian response");
    }

    printf("\nEndianness test (Server->Client): %s\n", srv_endian_errors == 0 ? "PASSED" : "FAILED");

    if (srv_endian_errors > 0) {
        tx_errors++;  // Count as TX error since it affects overall result
    }

    // UDP echo test: exact frame sizes, both directions
    printf("\nFrame size test (UDP echo, %d attempts each)...\n", UDP_ATTEMPTS);
    {
        int udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
        unsigned char *udp_tx = malloc(1472);

        if (udp_fd < 0 || !udp_tx) {
            printf("  ERROR: cannot set up UDP test\n");
            udp_errors++;
        } else {
            for (size_t i = 0; i < NUM_UDP_SIZES; i++) {
                size_t size = udp_test_sizes[i];
                int ok = 0;

                for (int attempt = 1; attempt <= UDP_ATTEMPTS && !ok; attempt++) {
                    // First two bytes identify the datagram, the rest varies per byte
                    udp_tx[0] = (unsigned char)i;
                    udp_tx[1] = (unsigned char)attempt;
                    for (size_t k = 2; k < size; k++) {
                        udp_tx[k] = (unsigned char)(i * 17 + (k % 251));
                    }

                    if (sendto(udp_fd, udp_tx, size, 0,
                               (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
                        break;
                    }

                    // Wait for the echo of this datagram; skip late echoes of earlier ones
                    for (;;) {
                        fd_set rfds;
                        struct timeval tv;

                        FD_ZERO(&rfds);
                        FD_SET(udp_fd, &rfds);
                        tv.tv_sec = UDP_TIMEOUT_SECS;
                        tv.tv_usec = 0;
                        if (WaitSelect(udp_fd + 1, &rfds, NULL, NULL, &tv, NULL) <= 0) {
                            break;  // timeout
                        }

                        LONG got = recv(udp_fd, buffer, BUFFER_SIZE, 0);
                        if (got < 2 || (unsigned char)buffer[0] != udp_tx[0] ||
                            (unsigned char)buffer[1] != udp_tx[1]) {
                            continue;
                        }
                        if ((size_t)got == size && memcmp(buffer, udp_tx, size) == 0) {
                            ok = 1;
                        } else {
                            printf("  %4zu-byte frame: echo differs (%ld bytes back)\n",
                                   size + UDP_HEADERS, (long)got);
                        }
                        break;
                    }
                }

                printf("  %4zu-byte frame: %s\n", size + UDP_HEADERS, ok ? "OK" : "LOST");
                if (!ok) {
                    udp_errors++;
                }
            }
        }

        if (udp_tx) {
            free(udp_tx);
        }
        if (udp_fd >= 0) {
            CloseSocket(udp_fd);
        }
    }
    printf("Frame size test: %s\n", udp_errors == 0 ? "PASSED" : "FAILED");

    if (!args[1]) {
        // Tell the server the UDP phase is over
        send(socket_fd, (char *)UDP_DONE, strlen(UDP_DONE), 0);
    } else {
        // Throughput benchmark: the server sends BENCH_BYTES, then we send the same amount
        struct DateStamp t0, t1;
        ULONG total, ticks;

        printf("\nThroughput benchmark (%lu KB each way, no disk)...\n", BENCH_BYTES / 1024);
        send(socket_fd, (char *)UDP_BENCH, strlen(UDP_BENCH), 0);

        total = 0;
        DateStamp(&t0);
        while (total < BENCH_BYTES) {
            LONG got = recv(socket_fd, buffer, BUFFER_SIZE, 0);
            if (got <= 0) {
                break;
            }
            total += got;
        }
        DateStamp(&t1);
        ticks = (t1.ds_Minute - t0.ds_Minute) * 3000 + t1.ds_Tick - t0.ds_Tick;
        if (total == BENCH_BYTES && ticks) {
            printf("  Receive: %lu KB/s (%lu.%lu s)\n", (total / 1024) * 50 / ticks, ticks / 50, (ticks % 50) / 5);
        } else {
            printf("  Receive: FAILED after %lu bytes\n", total);
            errors++;
        }

        if (total == BENCH_BYTES) {
            total = 0;
            DateStamp(&t0);
            while (total < BENCH_BYTES) {
                LONG sent = send(socket_fd, buffer, BUFFER_SIZE, 0);
                if (sent <= 0) {
                    break;
                }
                total += sent;
            }
            // Wait until the server has it all
            if (total == BENCH_BYTES && recv(socket_fd, buffer, BUFFER_SIZE, 0) > 0) {
                DateStamp(&t1);
                ticks = (t1.ds_Minute - t0.ds_Minute) * 3000 + t1.ds_Tick - t0.ds_Tick;
                if (ticks) {
                    printf("  Send:    %lu KB/s (%lu.%lu s)\n", (total / 1024) * 50 / ticks, ticks / 50, (ticks % 50) / 5);
                }
            } else {
                printf("  Send: FAILED after %lu bytes\n", total);
                tx_errors++;
            }
        }
    }

final_cleanup:
    printf("\n");
    printf("Overall: RX=%d errors, TX=%d errors, frame size=%d errors\n", errors, tx_errors, udp_errors);

    free(buffer);
    CloseSocket(socket_fd);
    CloseLibrary(SocketBase);
    FreeArgs(rdargs);

    return (errors > 0 || tx_errors > 0 || udp_errors > 0) ? 1 : 0;
}
