#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <proto/exec.h>
#include <proto/bsdsocket.h>
#include <proto/dos.h>

// ignore that STRPTR is unsigned char *, but all string literals are char *
#pragma GCC diagnostic ignored "-Wpointer-sign"

#define PORT 10203
#define BUFFER_SIZE 1024
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

    res = connect(socket_fd, (struct sockaddr *)&server_addr, sizeof(server_addr));
    if (res < 0) {
        PrintFault(IoErr(), "connect");
        CloseSocket(socket_fd);
        CloseLibrary(SocketBase);
        FreeArgs(rdargs);
        return 1;
    }

    // Send a well-defined message to the server
    ssize_t bytes_written = send(socket_fd, (char *)MESSAGE, strlen(MESSAGE), 0);
    if (bytes_written < 0) {
        PrintFault(IoErr(), "send");
        CloseSocket(socket_fd);
        CloseLibrary(SocketBase);
        FreeArgs(rdargs);
        return 1;
    }

    // Receive the response from the server
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
    printf("Received: %s\n", buffer);

    // Check if the response matches the expected response
    if (strcmp(buffer, EXPECTED_RESPONSE) == 0) {
        printf("Received expected response from server.\n");
    } else {
        printf("Unexpected response from server.\n");
    }

    CloseSocket(socket_fd);
    CloseLibrary(SocketBase);
    FreeArgs(rdargs);

    return 0;
}