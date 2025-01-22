#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#define BUFFER_SIZE 4000

void send_request(const char *server_ip, int port, const char *request) {
    int sock;
    struct sockaddr_in server_addr;
    char response[BUFFER_SIZE];

    // Create socket
    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("Socket creation error");
        return;
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);

    // Convert IPv4 and IPv6 addresses from text to binary
    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) {
        perror("Invalid address or Address not supported");
        close(sock);
        return;
    }

    // Connect to the server
    if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Connection Failed");
        close(sock);
        return;
    }

    // Send the request
    send(sock, request, strlen(request), 0);

    // Receive the response
    ssize_t bytes_read = read(sock, response, sizeof(response) - 1);
    if (bytes_read > 0) {
        response[bytes_read] = '\0';
        printf("Response:\n%s\n", response);
    } else {
        perror("Read error");
    }

    close(sock);
}

int main() {
    const char *server_ip = "127.0.0.1";
    int port = 8080;

    // Test cases
    const char *test_cases[] = {
            "GET /index.html HTTP/1.1\r\nHost: localhost\r\n\r\n",         // Valid GET request
            "POST /index.html HTTP/1.1\r\nHost: localhost\r\n\r\n",        // Invalid method
            "GET /nonexistent.html HTTP/1.1\r\nHost: localhost\r\n\r\n",  // Nonexistent file
            "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n",                  // Directory request
            "GET / HTTP/1.1\r\nHost: localhost\r\nExtra: value\r\n\r\n",  // Bad request
    };

    // Run tests
    for (int i = 0; i < sizeof(test_cases) / sizeof(test_cases[0]); ++i) {
        printf("Test Case %d:\n", i + 1);
        send_request(server_ip, port, test_cases[i]);
        printf("\n");
    }

    return 0;
}