#include "proxy.h"
#include "log.h"
#include "buffer.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>

#define DEFAULT_HTTP_PORT   80
#define CONNECT_TIMEOUT_SEC 30
#define IO_TIMEOUT_SEC      60

static int setNonBlocking(int sock)
{
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0)
    {
        return ERROR;
    }
    return fcntl(sock, F_SETFL, flags | O_NONBLOCK);
}

static int setBlocking(int sock)
{
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0)
    {
        return ERROR;
    }
    return fcntl(sock, F_SETFL, flags & ~O_NONBLOCK);
}

static int getSocketError(int sock)
{
    int error = 0;
    socklen_t len = sizeof(error);
    
    if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &len) < 0)
    {
        return errno;
    }
    return error;
}

static int waitForWritable(int sock, int timeoutSec)
{
    fd_set writefds;
    struct timeval timeout;

    FD_ZERO(&writefds);
    FD_SET(sock, &writefds);

    timeout.tv_sec = timeoutSec;
    timeout.tv_usec = 0;

    int result = select(sock + 1, NULL, &writefds, NULL, &timeout);

    if (result < 0)
    {
        return ERROR;
    }
    if (result == 0)
    {
        errno = ETIMEDOUT;
        return ERROR;
    }

    return SUCCESS;
}

int waitForReadable(int sock, int timeoutSec)
{
    fd_set readfds;
    struct timeval timeout;

    FD_ZERO(&readfds);
    FD_SET(sock, &readfds);

    timeout.tv_sec = timeoutSec;
    timeout.tv_usec = 0;

    int result = select(sock + 1, &readfds, NULL, NULL, &timeout);

    if (result < 0)
    {
        return ERROR;
    }
    if (result == 0)
    {
        errno = ETIMEDOUT;
        return ERROR;
    }

    return SUCCESS;
}

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

int connectToHost(const char *host, int port)
{
    int sock = -1;
    int error = 0;

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

    if (setNonBlocking(sock) < 0)
    {
        logError("Failed to set non-blocking mode");
        goto cleanup;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    memcpy(&addr.sin_addr.s_addr, server->h_addr, server->h_length);

    int result = connect(sock, (struct sockaddr *)&addr, sizeof(addr));

    if (result < 0)
    {
        if (errno != EINPROGRESS)
        {
            logError("Failed to initiate connection");
            goto cleanup;
        }

        if (waitForWritable(sock, CONNECT_TIMEOUT_SEC) != SUCCESS)
        {
            if (errno == ETIMEDOUT)
            {
                logError("Connection timed out");
            }
            else
            {
                logError("Connection wait failed");
            }
            goto cleanup;
        }

        error = getSocketError(sock);
        if (error != 0)
        {
            logError("Connection failed");
            goto cleanup;
        }
    }

    if (setBlocking(sock) < 0)
    {
        logError("Failed to set blocking mode");
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
        if (waitForWritable(socket, IO_TIMEOUT_SEC) != SUCCESS)
        {
            if (errno == ETIMEDOUT)
            {
                logError("Send timed out");
            }
            else
            {
                logError("Send wait failed");
            }
            return ERROR;
        }

        ssize_t n = send(socket, data + sent, size - sent, 0);
        if (n < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                continue; 
            }
            logError("Send failed");
            return ERROR;
        }
        if (n == 0)
        {
            logError("Connection closed during send");
            return ERROR;
        }

        sent += n;
    }

    return sent;
}

ssize_t recvWithTimeout(int socket, char *buffer, size_t size, int timeoutSec)
{
    if (waitForReadable(socket, timeoutSec) != SUCCESS)
    {
        if (errno == ETIMEDOUT)
        {
            logError("Receive timed out");
        }
        else
        {
            logError("Receive wait failed");
        }
        return ERROR;
    }

    ssize_t n = recv(socket, buffer, size, 0);

    if (n < 0)
    {
        if (errno != EAGAIN)
        {
            logError("Receive failed");
        }
        return ERROR;
    }

    return n;
}

ssize_t recvToBuffer(int socket, Buffer *buffer)
{
    if (Buffer_available(buffer) == 0)
    {
        size_t currentCap = get_Buffer_capacity(buffer);

        size_t newCapacity = currentCap * 2;

        if (Buffer_reserve(buffer, newCapacity) != 0)
        {
            logError("Failed to expand buffer");
            return ERROR;
        }

        logDebug("Buffer expanded");
    }

    char *ptr = Buffer_writePtr(buffer);
    size_t available = Buffer_available(buffer);

    ssize_t n = recvWithTimeout(socket, ptr, available, IO_TIMEOUT_SEC);

    if (n < 0)
    {
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

    while (1)
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
        sendAll(sock, response, len);
    }
}