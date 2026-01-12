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

#define PORT 10203
#define BUFFER_SIZE 8192
#define EXPECTED_MESSAGE "Hello, Server!"
#define RESPONSE "Hello, Client!"

// Test packet sizes to exercise different buffer scenarios
static const size_t test_sizes[] = {
    64,      // Minimum Ethernet payload
    100,     // Small packet
    256,     // 1 page
    512,     // 2 pages
    1024,    // 4 pages
    1518,    // Maximum Ethernet frame (including header)
    2048,    // Large packet
    4096     // Very large packet
};

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
            test_buffer[j] = pattern;
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

    printf("All test packets sent.\n");

    close(client_socket_fd);
    close(socket_fd);

    return 0;
}