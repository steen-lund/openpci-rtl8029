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
#define BUFFER_SIZE 1024
#define EXPECTED_MESSAGE "Hello, Server!"
#define RESPONSE "Hello, Client!"

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

    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    client_socket_fd = accept(socket_fd, (struct sockaddr *)&client_addr, &client_addr_len);
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