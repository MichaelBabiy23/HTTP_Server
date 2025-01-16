#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <netinet/in.h>
#include <errno.h>
#include <signal.h>
#include "threadpool.h"

#define MAX_LINE_LENGTH 4000  // Max length of the request line
#define BACKLOG 5             // Maximum length of the connection queue

// Error messages
#define USAGE_ERROR "Usage: server <port> <pool-size> <queue-size> <max-requests>\n"

void handle_request(void* arg);

int main(int argc, char* argv[]) {
    if (argc != 5) {
        fprintf(stderr, USAGE_ERROR);
        exit(EXIT_FAILURE);
    }

    // Parse command-line arguments
    int port = atoi(argv[1]);
    int pool_size = atoi(argv[2]);
    int max_queue_size = atoi(argv[3]);
    int max_requests = atoi(argv[4]);

    if (port <= 0 || pool_size <= 0 || max_queue_size <= 0 || max_requests <= 0) {
        fprintf(stderr, USAGE_ERROR);
        exit(EXIT_FAILURE);
    }

    // Initialize the thread pool
    struct _threadpool_st* pool = create_threadpool(pool_size, max_queue_size);
    if (!pool) {
        perror("Failed to initialize thread pool");
        exit(EXIT_FAILURE);
    }

    // Set up the server socket
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        destroy_threadpool(pool);
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(port);

    // Bind the socket
    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        close(server_fd);
        destroy_threadpool(pool);
        exit(EXIT_FAILURE);
    }

    // Listen for connections
    if (listen(server_fd, BACKLOG) < 0) {
        perror("listen");
        close(server_fd);
        destroy_threadpool(pool);
        exit(EXIT_FAILURE);
    }

    printf("Server is running on port %d...\n", port);

    // Accept and handle connections
    int handled_requests = 0;
    while (handled_requests < max_requests) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) {
            perror("accept");
            continue;
        }

        // Dispatch the client connection as a task
        dispatch(pool, handle_request, &client_fd);

        handled_requests++;
    }

    // Clean up resources
    destroy_threadpool(pool);
    close(server_fd);
    printf("Server shut down.\n");

    return 0;
}

void handle_request(void* arg) {
    int client_fd = *(int*)arg;
    char buffer[MAX_LINE_LENGTH];
    memset(buffer, 0, sizeof(buffer));

    // Read the request line
    ssize_t bytes_read = read(client_fd, buffer, sizeof(buffer) - 1);
    if (bytes_read <= 0) {
        perror("read");
        close(client_fd);
        return;
    }

    printf("Request: %s\n", buffer);

    // TODO: Parse the request, validate the method and path,
    //       and generate an appropriate HTTP response.

    close(client_fd);
}

