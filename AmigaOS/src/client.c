#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
        bytes_read = recv(socket_fd, buffer, BUFFER_SIZE - 1, 0);
        if (bytes_read <= 0) {
            if (bytes_read == 0) {
                printf("Connection closed by server.\n");
            } else {
                PrintFault(IoErr(), "recv");
            }
            break;
        }

        packet_num++;
        buffer[bytes_read] = '\0';

        // Parse size from header
        size_t expected_size = 0;
        if (sscanf(buffer, "SIZE:%zu:", &expected_size) == 1) {
            printf("Packet %d: Received %d bytes (expected %zu)\n",
                   packet_num, (int)bytes_read, expected_size);

            // Verify size matches
            if ((size_t)bytes_read != expected_size) {
                printf("  ERROR: Size mismatch!\n");
                errors++;
            } else {
                // Verify pattern in payload
                size_t header_len = 0;
                for (size_t i = 0; i < (size_t)bytes_read && buffer[i] != ':'; i++) {
                    if (i > 0 && buffer[i-1] == ':') {
                        header_len = i;
                        break;
                    }
                }
                if (header_len == 0) header_len = strlen(buffer);

                // Check pattern consistency
                int pattern_ok = 1;
                if (header_len < (size_t)bytes_read) {
                    unsigned char expected_pattern = buffer[header_len];
                    for (size_t i = header_len + 1; i < (size_t)bytes_read; i++) {
                        if ((unsigned char)buffer[i] != expected_pattern) {
                            pattern_ok = 0;
                            break;
                        }
                    }
                }

                if (pattern_ok) {
                    printf("  OK: Pattern verified (0x%02X)\n",
                           header_len < (size_t)bytes_read ? (unsigned char)buffer[header_len] : 0);
                } else {
                    printf("  ERROR: Pattern mismatch!\n");
                    errors++;
                }
            }
        } else {
            printf("Packet %d: Received %d bytes (unknown format)\n",
                   packet_num, (int)bytes_read);
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

    printf("\n");
    printf("Test complete: %d packets received, %d errors\n", packet_num, errors);

    CloseSocket(socket_fd);
    CloseLibrary(SocketBase);
    FreeArgs(rdargs);

    return errors > 0 ? 1 : 0;
}