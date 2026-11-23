/*
 * server - host side of the openpci-rtl8029.device network test
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
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#include <ifaddrs.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/time.h>

#define PORT 10203
#define BUFFER_SIZE 8192
#define EXPECTED_MESSAGE "Hello, Server!"
#define RESPONSE "Hello, Client!"
#define UDP_DONE "UDP-DONE"
// Optional throughput benchmark, requested by the client instead of UDP_DONE
#define UDP_BENCH "UDP-BENCH"
#define BENCH_DONE "BENCH-DONE"
#define BENCH_BYTES (4UL * 1024 * 1024)   // per direction; must match the client
#define UDP_HEADERS 42  // Ethernet + IP + UDP headers around one datagram

// Test packet sizes to exercise different buffer scenarios
static const size_t test_sizes[] = {
    // Odd and even sizes are both needed: the NE2000 moves data in 16-bit words,
    // so odd-length frames take a different path in the driver.
    // Must match tx_test_sizes in the Amiga client.
    64, 65, 100, 101, 256, 333, 512, 1001, 1024, 1459, 1461, 1518, 2048, 4096
};
#define NUM_TEST_SIZES (sizeof(test_sizes) / sizeof(test_sizes[0]))

int socket_fd = -1;
int client_socket_fd = -1;

void handle_sigint(int sig __attribute__((unused))) {
    if (client_socket_fd != -1) {
        close(client_socket_fd);
    }
    if (socket_fd != -1) {
        close(socket_fd);
    }
    printf("Server shutting down...\n");
    exit(0);
}

static double seconds_now(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

// Throughput benchmark: send BENCH_BYTES to the client, then receive the same amount
static void run_benchmark(int fd) {
    static char chunk[65536];
    size_t total;
    double t0;

    printf("\nThroughput benchmark (%lu KB each way)...\n", BENCH_BYTES / 1024);
    for (size_t i = 0; i < sizeof(chunk); i++) {
        chunk[i] = (char)(i * 31 + (i >> 8));
    }

    t0 = seconds_now();
    for (total = 0; total < BENCH_BYTES; ) {
        size_t want = BENCH_BYTES - total < sizeof(chunk) ? BENCH_BYTES - total : sizeof(chunk);
        ssize_t sent = write(fd, chunk, want);
        if (sent <= 0) {
            perror("benchmark write");
            return;
        }
        total += sent;
    }
    printf("  Sent to client in %.1f s (rate as seen by the sender, the client's figure is the real one)\n",
           seconds_now() - t0);

    t0 = seconds_now();
    for (total = 0; total < BENCH_BYTES; ) {
        ssize_t got = read(fd, chunk, sizeof(chunk));
        if (got <= 0) {
            printf("  Receive from client failed after %zu bytes\n", total);
            return;
        }
        total += got;
    }
    printf("  Received from client: %.0f KB/s (%.1f s)\n",
           (BENCH_BYTES / 1024.0) / (seconds_now() - t0), seconds_now() - t0);
    if (write(fd, BENCH_DONE, strlen(BENCH_DONE)) != (ssize_t)strlen(BENCH_DONE))
        printf("  Could not tell the client the benchmark is done\n");
}

void print_ip_address() {
    struct ifaddrs *ifaddr, *ifa;
    char ip[INET_ADDRSTRLEN];

    if (getifaddrs(&ifaddr) == -1) {
        perror("getifaddrs");
        exit(EXIT_FAILURE);
    }

    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL)
            continue;

        if (ifa->ifa_addr->sa_family == AF_INET) {
            struct sockaddr_in *sa = (struct sockaddr_in *)ifa->ifa_addr;
            inet_ntop(AF_INET, &(sa->sin_addr), ip, INET_ADDRSTRLEN);
            printf("Interface: %s\tAddress: %s\n", ifa->ifa_name, ip);
        }
    }

    freeifaddrs(ifaddr);
}

int main()
{
    struct sockaddr_in addr;
    int res;

    // Register the signal handler for SIGINT
    signal(SIGINT, handle_sigint);

    socket_fd = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_fd < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    res = bind(socket_fd, (struct sockaddr *)&addr, sizeof(addr));
    if (res < 0) {
        perror("bind");
        close(socket_fd);
        exit(EXIT_FAILURE);
    }

    // UDP echo socket for the frame size test, same port number
    int udp_fd = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udp_fd < 0 || bind(udp_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("udp socket");
        close(socket_fd);
        exit(EXIT_FAILURE);
    }

    res = listen(socket_fd, 5);
    if (res < 0) {
        perror("listen");
        close(socket_fd);
        exit(EXIT_FAILURE);
    }

    print_ip_address();
    printf("Waiting for client connection...\n");

    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    client_socket_fd = accept(socket_fd, (struct sockaddr *)&client_addr, &client_addr_len);
    if (client_socket_fd < 0) {
        perror("accept");
        close(socket_fd);
        exit(EXIT_FAILURE);
    }

    printf("Client connected from %s\n", inet_ntoa(client_addr.sin_addr));

    char buffer[BUFFER_SIZE];

    // Initial handshake
    ssize_t bytes_read = read(client_socket_fd, buffer, BUFFER_SIZE - 1);
    if (bytes_read < 0) {
        perror("read");
        close(client_socket_fd);
        close(socket_fd);
        exit(EXIT_FAILURE);
    }

    buffer[bytes_read] = '\0';
    printf("Received handshake: %s (%zd bytes)\n", buffer, bytes_read);

    if (strcmp(buffer, EXPECTED_MESSAGE) != 0) {
        printf("Unexpected handshake message.\n");
        close(client_socket_fd);
        close(socket_fd);
        exit(EXIT_FAILURE);
    }

    // Respond to handshake
    ssize_t bytes_written = write(client_socket_fd, RESPONSE, strlen(RESPONSE));
    if (bytes_written < 0) {
        perror("write");
        close(client_socket_fd);
        close(socket_fd);
        exit(EXIT_FAILURE);
    }
    printf("Handshake complete.\n\n");

    // Send test packets of various sizes
    size_t num_tests = sizeof(test_sizes) / sizeof(test_sizes[0]);
    for (size_t i = 0; i < num_tests; i++) {
        size_t size = test_sizes[i];

        // Create a test buffer filled with a pattern
        char *test_buffer = malloc(size);
        if (!test_buffer) {
            perror("malloc");
            break;
        }

        // Fill with pattern: size info + repeating pattern
        snprintf(test_buffer, size, "SIZE:%zu:", size);
        size_t header_len = strlen(test_buffer);

        // Fill rest with a repeating byte pattern based on packet number
        unsigned char pattern = 0x41 + (i % 26);  // 'A' to 'Z'
        for (size_t j = header_len; j < size; j++) {
            test_buffer[j] = (char)(pattern + ((j - header_len) % 251));
        }

        printf("Sending test packet %zu/%zu: %zu bytes (pattern: 0x%02X)...\n",
               i + 1, num_tests, size, pattern);

        // Send length prefix (4 bytes, network byte order)
        uint32_t net_size = htonl((uint32_t)size);
        bytes_written = write(client_socket_fd, &net_size, sizeof(net_size));
        if (bytes_written < 0) {
            perror("write length");
            free(test_buffer);
            break;
        }

        // Send the actual data
        bytes_written = write(client_socket_fd, test_buffer, size);
        if (bytes_written < 0) {
            perror("write");
            free(test_buffer);
            break;
        }
        if ((size_t)bytes_written != size) {
            printf("Warning: only sent %zd of %zu bytes\n", bytes_written, size);
        }

        free(test_buffer);

        // Wait for acknowledgment
        bytes_read = read(client_socket_fd, buffer, BUFFER_SIZE - 1);
        if (bytes_read < 0) {
            perror("read");
            break;
        }
        buffer[bytes_read] = '\0';
        printf("  Client ACK: %s\n\n", buffer);

        // Small delay between packets
        usleep(100000);  // 100ms
    }

    printf("All test packets sent.\n\n");
    printf("Now receiving test packets from client...\n\n");

    // Receive test packets from client
    int rx_packet_num = 0;
    int rx_errors = 0;
    const int expected_rx_packets = NUM_TEST_SIZES;  // Client sends the same set of sizes

    while (rx_packet_num < expected_rx_packets) {
        // Read length prefix (4 bytes)
        uint32_t net_size;
        size_t total_read = 0;
        while (total_read < sizeof(net_size)) {
            bytes_read = read(client_socket_fd, ((char*)&net_size) + total_read,
                            sizeof(net_size) - total_read);
            if (bytes_read <= 0) {
                if (bytes_read == 0) {
                    if (total_read == 0) {
                        printf("Client closed connection.\n");
                        goto done;
                    }
                    printf("Connection closed mid-length!\n");
                } else {
                    perror("read length");
                }
                goto done;
            }
            total_read += bytes_read;
        }

        size_t expected_size = ntohl(net_size);
        if (expected_size > BUFFER_SIZE - 1) {
            printf("Packet too large: %zu bytes\n", expected_size);
            rx_errors++;
            break;
        }

        // Read the complete packet data
        total_read = 0;
        while (total_read < expected_size) {
            bytes_read = read(client_socket_fd, buffer + total_read,
                            expected_size - total_read);
            if (bytes_read <= 0) {
                if (bytes_read == 0) {
                    printf("Connection closed mid-packet!\n");
                } else {
                    perror("read data");
                }
                goto done;
            }
            total_read += bytes_read;
        }

        rx_packet_num++;
        buffer[total_read] = '\0';

        // Parse and verify
        size_t header_size = 0;
        if (sscanf(buffer, "SIZE:%zu:", &header_size) == 1) {
            printf("RX Packet %d: Received %zu bytes (expected %zu)\n",
                   rx_packet_num, total_read, header_size);

            if (total_read != expected_size || expected_size != header_size) {
                printf("  ERROR: Size mismatch!\n");
                rx_errors++;
            } else {
                // Verify pattern
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
                if (header_len == 0) header_len = total_read;

                int pattern_ok = 1;
                if (header_len < total_read) {
                    unsigned char expected_pattern = buffer[header_len];
                    for (size_t i = header_len + 1; i < total_read; i++) {
                        if ((unsigned char)buffer[i] != (unsigned char)(expected_pattern + ((i - header_len) % 251))) {
                            pattern_ok = 0;
                            break;
                        }
                    }

                    if (pattern_ok) {
                        printf("  OK: Pattern verified (0x%02X)\n", expected_pattern);
                    } else {
                        printf("  ERROR: Pattern mismatch!\n");
                        rx_errors++;
                    }
                }
            }
        }

        // Send acknowledgment
        char ack[32];
        snprintf(ack, sizeof(ack), "RX-ACK:%d", rx_packet_num);
        ssize_t bytes_written = write(client_socket_fd, ack, strlen(ack));
        if (bytes_written < 0) {
            perror("write ack");
            break;
        }
    }

done:
    printf("\n");
    printf("Client TX test complete: %d packets received, %d errors\n", rx_packet_num, rx_errors);

    // Endianness test - receive and verify structured binary data
    printf("\nEndianness test: Receiving binary data...\n");

    // Read length prefix
    uint32_t net_size;
    size_t total_read = 0;
    while (total_read < sizeof(net_size)) {
        bytes_read = read(client_socket_fd, ((char*)&net_size) + total_read,
                        sizeof(net_size) - total_read);
        if (bytes_read <= 0) {
            printf("Failed to read endian test length\n");
            goto cleanup;
        }
        total_read += bytes_read;
    }

    size_t expected_size = ntohl(net_size);
    printf("  Expecting %zu bytes of binary data\n", expected_size);

    // Read data
    char binary_buffer[256];
    total_read = 0;
    while (total_read < expected_size) {
        bytes_read = read(client_socket_fd, binary_buffer + total_read,
                        expected_size - total_read);
        if (bytes_read <= 0) {
            printf("Failed to read endian test data\n");
            goto cleanup;
        }
        total_read += bytes_read;
    }

    // Verify the data
    struct __attribute__((packed)) {
        uint32_t magic;
        uint16_t version;
        uint32_t counter;
        float value;
        uint64_t timestamp;
    } *test_data = (void*)binary_buffer;

    printf("  Received %zu bytes, expected struct size: %zu\n", total_read, sizeof(*test_data));
    printf("  Raw bytes: ");
    for (size_t i = 0; i < total_read && i < 32; i++) {
        printf("%02X ", (unsigned char)binary_buffer[i]);
    }
    printf("\n");

    uint32_t magic = ntohl(test_data->magic);
    uint16_t version = ntohs(test_data->version);
    uint32_t counter = ntohl(test_data->counter);

    // Float conversion
    union { float f; uint32_t i; } float_conv;
    float_conv.f = test_data->value;
    float_conv.i = ntohl(float_conv.i);
    float value = float_conv.f;

    // 64-bit conversion - read as two 32-bit values in network byte order
    // The bytes are: 01 23 45 67 89 AB CD EF (high word first, then low word)
    // Struct layout: magic(4) + version(2) + counter(4) + value(4) + timestamp(8) = 22 bytes
    // Timestamp starts at offset 14
    uint32_t ts_high, ts_low;
    memcpy(&ts_high, &binary_buffer[14], 4);  // Offset 14 is where timestamp starts
    memcpy(&ts_low, &binary_buffer[18], 4);   // Next 4 bytes
    ts_high = ntohl(ts_high);
    ts_low = ntohl(ts_low);
    uint64_t timestamp = ((uint64_t)ts_high << 32) | ts_low;

    int endian_errors = 0;
    printf("  Verifying received data:\n");
    printf("    magic:     0x%08X (expected 0xDEADBEEF) %s\n", magic,
           magic == 0xDEADBEEF ? "✓" : "✗");
    if (magic != 0xDEADBEEF) endian_errors++;

    printf("    version:   0x%04X (expected 0x1234) %s\n", version,
           version == 0x1234 ? "✓" : "✗");
    if (version != 0x1234) endian_errors++;

    printf("    counter:   %u (expected 12345678) %s\n", counter,
           counter == 12345678 ? "✓" : "✗");
    if (counter != 12345678) endian_errors++;

    printf("    value:     %.5f (expected 3.14159) %s\n", value,
           (value > 3.14158 && value < 3.14160) ? "✓" : "✗");
    if (!(value > 3.14158 && value < 3.14160)) endian_errors++;

    printf("    timestamp: 0x%016llX (expected 0x0123456789ABCDEF) %s\n",
           (unsigned long long)timestamp,
           timestamp == 0x0123456789ABCDEFULL ? "✓" : "✗");
    if (timestamp != 0x0123456789ABCDEFULL) endian_errors++;

    // Send response
    char response[128];
    if (endian_errors == 0) {
        snprintf(response, sizeof(response), "ENDIAN-OK: All values correct!");
    } else {
        snprintf(response, sizeof(response), "ENDIAN-ERROR: %d values incorrect", endian_errors);
    }
    if (write(client_socket_fd, response, strlen(response)) != (ssize_t)strlen(response))
        printf("Warning: could not send the endianness result to the client\n");

    printf("\nEndianness test (Client→Server): %s\n", endian_errors == 0 ? "PASSED" : "FAILED");

    // Now send endianness test data back to client
    printf("\nSending endianness test data to client...\n");

    struct __attribute__((packed)) {
        uint32_t magic;      // 0xCAFEBABE
        uint16_t version;    // 0x5678
        uint32_t counter;    // 87654321
        float value;         // 2.71828
        uint64_t timestamp;  // 0xFEDCBA9876543210
    } server_test_data;

    server_test_data.magic = htonl(0xCAFEBABE);
    server_test_data.version = htons(0x5678);
    server_test_data.counter = htonl(87654321);

    // Float requires special handling
    union { float f; uint32_t i; } srv_float_conv;
    srv_float_conv.f = 2.71828f;
    srv_float_conv.i = htonl(srv_float_conv.i);
    server_test_data.value = srv_float_conv.f;

    // 64-bit value - write high word first in memory, then low word
    // On little-endian x86, we need to swap the order
    server_test_data.timestamp = ((uint64_t)htonl(0x76543210) << 32) | htonl(0xFEDCBA98);

    printf("  Size of test_data: %zu bytes\n", sizeof(server_test_data));

    // Send length prefix
    net_size = htonl((uint32_t)sizeof(server_test_data));
    bytes_written = write(client_socket_fd, &net_size, sizeof(net_size));
    if (bytes_written < 0) {
        perror("send endian test length");
    } else {
        // Send data
        bytes_written = write(client_socket_fd, &server_test_data, sizeof(server_test_data));
        if (bytes_written < 0) {
            perror("send endian test data");
        } else {
            printf("  Sent %zd bytes of binary data\n", bytes_written);

            // Wait for verification response
            bytes_read = read(client_socket_fd, buffer, BUFFER_SIZE - 1);
            if (bytes_read > 0) {
                buffer[bytes_read] = '\0';
                printf("  Client response: %s\n", buffer);
            }
        }
    }

    // Frame size test: echo every UDP datagram back unchanged until the client says it is
    // done over TCP (or closes the connection).
    printf("\nFrame size test: echoing UDP datagrams...\n");
    for (;;) {
        fd_set rfds;
        struct timeval tv = { 30, 0 };
        int maxfd = udp_fd > client_socket_fd ? udp_fd : client_socket_fd;

        FD_ZERO(&rfds);
        FD_SET(udp_fd, &rfds);
        FD_SET(client_socket_fd, &rfds);
        if (select(maxfd + 1, &rfds, NULL, NULL, &tv) <= 0) {
            printf("  No traffic for 30 seconds, giving up.\n");
            break;
        }

        if (FD_ISSET(udp_fd, &rfds)) {
            struct sockaddr_in from;
            socklen_t from_len = sizeof(from);
            ssize_t got = recvfrom(udp_fd, buffer, BUFFER_SIZE, 0, (struct sockaddr *)&from, &from_len);
            if (got > 0) {
                printf("  %4zd-byte frame received, echoing\n", got + UDP_HEADERS);
                sendto(udp_fd, buffer, got, 0, (struct sockaddr *)&from, from_len);
            }
        }

        if (FD_ISSET(client_socket_fd, &rfds)) {
            char done[32];
            ssize_t got = read(client_socket_fd, done, sizeof(done) - 1);
            if (got <= 0) {
                printf("Client closed connection.\n");
            } else {
                printf("Frame size test finished.\n");
                done[got] = '\0';
                if (strncmp(done, UDP_BENCH, strlen(UDP_BENCH)) == 0) {
                    run_benchmark(client_socket_fd);
                }
            }
            break;
        }
    }

cleanup:
    close(udp_fd);
    close(client_socket_fd);
    close(socket_fd);

    return 0;
}