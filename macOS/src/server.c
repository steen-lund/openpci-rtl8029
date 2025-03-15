#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#define PORT 10203
#define BUFFER_SIZE 1024
#define EXPECTED_MESSAGE "Hello, Server!"
#define RESPONSE "Hello, Client!"

int main()
{
    struct sockaddr_in addr;
    int res;

    int socket_fd = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
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

    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    int client_socket_fd = accept(socket_fd, (struct sockaddr *)&client_addr, &client_addr_len);
    if (client_socket_fd < 0) {
        perror("accept");
        close(socket_fd);
        exit(EXIT_FAILURE);
    }

    char buffer[BUFFER_SIZE];
    ssize_t bytes_read = read(client_socket_fd, buffer, BUFFER_SIZE - 1);
    if (bytes_read < 0) {
        perror("read");
    } else {
        buffer[bytes_read] = '\0';
        printf("Received: %s\n", buffer);

        // Check if the received message matches the expected message
        if (strcmp(buffer, EXPECTED_MESSAGE) == 0) {
            // Respond with the well-defined response
            ssize_t bytes_written = write(client_socket_fd, RESPONSE, strlen(RESPONSE));
            if (bytes_written < 0) {
                perror("write");
            }
        } else {
            printf("Unexpected message received.\n");
        }
    }

    close(client_socket_fd);
    close(socket_fd);

    return 0;
}