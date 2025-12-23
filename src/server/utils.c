#include "proxy.h"
#include "log.h"
#include "buffer.h"

#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

#define DEFAULT_HTTP_PORT 80

int parseUrl(const char *url, char *host, char *path, int *port)
{
    *port = DEFAULT_HTTP_PORT;
    path[0] = '\0';

    if (sscanf(url, "http://%1023[^:/]:%d/%2047[^\n]", host, port, path) == 3)
    {
        return SUCCESS;
    }
    if (sscanf(url, "http://%1023[^/]/%2047[^\n]", host, path) == 2)
    {
        return SUCCESS;
    }
    if (sscanf(url, "http://%1023[^:/]:%d", host, port) == 2)
    {
        return SUCCESS;
    }
    if (sscanf(url, "http://%1023[^\n/]", host) == 1)
    {
        return SUCCESS;
    }

    logError("Failed to parse URL");
    return ERROR;
}

static int findHeaderEnd(Buffer *buffer)
{
    const char *data = get_Buffer_data(buffer);
    size_t len = get_Buffer_size(buffer);

    for (size_t i = 0; i + 3 < len; i++)
    {
        if (data[i] == '\r' && data[i + 1] == '\n' &&
            data[i + 2] == '\r' && data[i + 3] == '\n')
        {
            return i + 4;
        }
    }
    return -1;
}

int isResponse200(const char *data)
{
    return (strncmp(data, "HTTP/1.1 200", 12) == 0 ||
            strncmp(data, "HTTP/1.0 200", 12) == 0);
}

int isGetRequest(const char *method)
{
    return strcmp(method, "GET") == 0;
}

int setSocketTimeout(int socket, int timeoutSec)
{
    struct timeval timeout;
    timeout.tv_sec = timeoutSec;
    timeout.tv_usec = 0;

    if (setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0)
    {
        logError("Failed to set receive timeout");
        return ERROR;
    }

    if (setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0)
    {
        logError("Failed to set send timeout");
        return ERROR;
    }

    return SUCCESS;
}

int connectToHost(const char *host, int port)
{
    int sock = -1;

    logDebug("Resolving host");

    struct hostent *server = gethostbyname(host);
    if (server == NULL)
    {
        logError("Failed to resolve host");
        return ERROR;
    }

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
    {
        logError("Failed to create socket");
        return ERROR;
    }

    if (setSocketTimeout(sock, SOCKET_TIMEOUT_SEC) != SUCCESS)
    {
        goto cleanup;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    memcpy(&addr.sin_addr.s_addr, server->h_addr, server->h_length);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        if (errno == ETIMEDOUT)
        {
            logError("Connection timed out");
        }
        else
        {
            logError("Failed to connect");
        }
        goto cleanup;
    }

    logDebug("Connected to remote host");
    return sock;

cleanup:
    if (sock >= 0)
    {
        close(sock);
    }
    return ERROR;
}

ssize_t sendAll(int socket, const char *data, size_t size)
{
    size_t sent = 0;
    while (sent < size)
    {
        ssize_t n = send(socket, data + sent, size - sent, 0);
        if (n <= 0)
        {
            if (errno == ETIMEDOUT)
            {
                logError("Send timed out");
            }
            else
            {
                logError("Send failed");
            }
            return ERROR;
        }
        sent += n;
    }

    return sent;
}

ssize_t recvToBuffer(int socket, Buffer *buffer)
{

    if (Buffer_available(buffer) < 1024)
    {
        size_t newCapacity = get_Buffer_capacity(buffer) * 2;
        if (Buffer_reserve(buffer, newCapacity) != 0)
        {
            logError("Failed to expand buffer");
            return ERROR;
        }
    }


    char *ptr = Buffer_writePtr(buffer);
    size_t available = Buffer_available(buffer);

    if (available == 0)
    {
        logError("Buffer is full");
        return ERROR;
    }

    ssize_t n = recv(socket, ptr, available, 0);

    if (n < 0)
    {
        if (errno == ETIMEDOUT || errno == EAGAIN || errno == EWOULDBLOCK)
        {
            logError("Receive timed out");
        }
        else
        {
            logError("Receive failed");
        }
        return ERROR;
    }

    if (n > 0)
    {
        Buffer_advanceSize(buffer, n);
    }

    return n;
}

ssize_t recvUntilHeaderEnd(int socket, Buffer *buffer)
{
    Buffer_clear(buffer);

    while (Buffer_available(buffer) > 0)
    {
        ssize_t n = recvToBuffer(socket, buffer);

        if (n < 0)
        {
            return ERROR;
        }

        if (n == 0)
        {
            logDebug("Connection closed by peer");
            break;
        }

        if (findHeaderEnd(buffer) >= 0)
        {
            break;
        }
    }

    return get_Buffer_size(buffer);
}

void sendErrorResponse(int sock, const char *status, const char *message)
{
    logDebug("Sending error response");

    char response[512];
    int len = snprintf(response, sizeof(response),
                       "HTTP/1.0 %s\r\n\r\n%s", status, message);

    if (len > 0)
    {
        if (sendAll(sock, response, len) < 0)
        {
            logError("Failed sendall");
        }
    }
}