#include "proxy.h"

#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

#define DEFAULT_HTTP_PORT 80


Buffer *Buffer_create(size_t capacity) {
    Buffer *buf = malloc(sizeof(Buffer));
    if (buf == NULL) {
        return NULL;
    }
    
    buf->data = malloc(capacity + 1);
    if (buf->data == NULL) {
        free(buf);
        return NULL;
    }
    
    buf->size = 0;
    buf->capacity = capacity;
    return buf;
}

void Buffer_destroy(Buffer *buffer) {
    if (buffer == NULL) return;
    free(buffer->data);
    free(buffer);
}

void Buffer_clear(Buffer *buffer) {
    buffer->size = 0;
}


int parseUrl(const char *url, char *host, char *path, int *port) {
    *port = DEFAULT_HTTP_PORT;
    path[0] = '\0';

    if (sscanf(url, "http://%1023[^:/]:%d/%2047[^\n]", host, port, path) == 3) {
        return SUCCESS;
    }
    if (sscanf(url, "http://%1023[^/]/%2047[^\n]", host, path) == 2) {
        return SUCCESS;
    }
    if (sscanf(url, "http://%1023[^:/]:%d", host, port) == 2) {
        return SUCCESS;
    }
    if (sscanf(url, "http://%1023[^\n/]", host) == 1) {
        return SUCCESS;
    }

    return ERROR;
}


int findHeaderEnd(const char *data, size_t len) {
    for (size_t i = 0; i + 3 < len; i++) {
        if (data[i] == '\r' && data[i+1] == '\n' && 
            data[i+2] == '\r' && data[i+3] == '\n') {
            return i + 4;
        }
    }
    return -1;
}

int isResponse200(const char *data) {
    return (strncmp(data, "HTTP/1.1 200", 12) == 0 ||
            strncmp(data, "HTTP/1.0 200", 12) == 0);
}

int isGetRequest(const char *method) {
    return strcmp(method, "GET") == 0;
}

ssize_t sendAll(int socket, const char *data, size_t size) {
    size_t sent = 0;
    while (sent < size) {
        ssize_t n = send(socket, data + sent, size - sent, 0);
        if (n <= 0) {
            return ERROR;
        }
        sent += n;
    }
    return sent;
}

ssize_t recvUntilHeaderEnd(int socket, Buffer *buffer) {
    Buffer_clear(buffer);
    
    while (buffer->size < buffer->capacity) {
        ssize_t n = recv(socket, buffer->data + buffer->size,
                         buffer->capacity - buffer->size, 0);
        if (n < 0) {
            return ERROR;
        }
        if (n == 0) {
            break;
        }
        
        buffer->size += n;
        buffer->data[buffer->size] = '\0';
        
        if (findHeaderEnd(buffer->data, buffer->size) >= 0) {
            break;
        }
    }
    return buffer->size;
}

int connectToHost(const char *host, int port) {
    struct hostent *server = gethostbyname(host);
    if (server == NULL) {
        return ERROR;
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return ERROR;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    memcpy(&addr.sin_addr.s_addr, server->h_addr, server->h_length);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        return ERROR;
    }

    return sock;
}

void sendErrorResponse(int sock, const char *status, const char *message) {
    char response[512];
    int len = snprintf(response, sizeof(response),
        "HTTP/1.0 %s\r\n\r\n%s", status, message);
    
    if (len > 0) {
        sendAll(sock, response, len);
    }
}