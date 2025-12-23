#include "proxy.h"
#include "log.h"
#include "buffer.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>

#define METHOD_MAX_LEN 16
#define URL_MAX_LEN 2048
#define PROTOCOL_MAX_LEN 16

static void waitForFirstChunk(CacheNodeT *node)
{
    CacheEntryT *entry = node->entry;

    pthread_mutex_lock(&entry->dataMutex);
    while (entry->status != Failed && entry->dataChunks == NULL)
    {
        pthread_cond_wait(&entry->dataCond, &entry->dataMutex);
    }
    pthread_mutex_unlock(&entry->dataMutex);
}

static void waitForMoreData(CacheEntryT *entry,
                            CacheEntryChunkT *chunk,
                            size_t currentSent)
{
    pthread_mutex_lock(&entry->dataMutex);
    while (entry->status == InProcess &&
           chunk->next == NULL &&
           chunk->curDataSize == currentSent)
    {
        pthread_cond_wait(&entry->dataCond, &entry->dataMutex);
    }
    pthread_mutex_unlock(&entry->dataMutex);
}

static int sendAllChunks(int clientSocket, CacheEntryT *entry)
{
    CacheEntryChunkT *chunk = entry->dataChunks;

    while (chunk != NULL)
    {
        size_t sent = 0;

        while (1)
        {
            if (entry->status == Failed)
            {
                logError("Cache entry failed during send");
                return ERROR;
            }

            size_t available = chunk->curDataSize;

            if (sent < available)
            {
                if (sendAll(clientSocket, chunk->data + sent, available - sent) < 0)
                {
                    logError("Failed to send chunk data");
                    return ERROR;
                }
                sent = available;
            }

            if (chunk->next != NULL || entry->status != InProcess)
            {
                break;
            }

            waitForMoreData(entry, chunk, sent);
        }

        chunk = chunk->next;
    }

    return (entry->status == Failed) ? ERROR : SUCCESS;
}

static int sendFromCache(int clientSocket, CacheNodeT *node)
{
    logDebug("Sending data from cache");

    waitForFirstChunk(node);

    if (node->entry->dataChunks == NULL)
    {
        logError("No data chunks available");
        return ERROR;
    }

    return sendAllChunks(clientSocket, node->entry);
}

static int forwardResponse(int clientSocket, int remoteSocket, Buffer *buffer)
{
    logDebug("Forwarding response without caching");

    while (1)
    {
        Buffer_clear(buffer);

        ssize_t n = recvToBuffer(remoteSocket, buffer);

        if (n <= 0)
        {
            break;
        }

        if (sendAll(clientSocket, get_Buffer_data(buffer), get_Buffer_size(buffer)) < 0)
        {
            logError("Failed to forward response");
            return ERROR;
        }
    }

    return SUCCESS;
}

static CacheNodeT *startDownload(Buffer *buffer,
                                 const char *host,
                                 int port,
                                 int clientSocket,
                                 const char *url,
                                 int *isCacheable)
{
    int remoteSocket = -1;
    CacheNodeT *node = NULL;
    CacheEntryT *entry = NULL;

    *isCacheable = 0;

    logDebug("Connecting to remote host");

    remoteSocket = connectToHost(host, port);
    if (remoteSocket < 0)
    {
        logError("Failed to connect to remote host");
        sendErrorResponse(clientSocket, HTTP_502_BAD_GATEWAY, "Failed to connect");
        return NULL;
    }

    logDebug("Sending request to remote");

    if (sendAll(remoteSocket, get_Buffer_data(buffer), get_Buffer_size(buffer)) < 0)
    {
        logError("Failed to send request to remote");
        sendErrorResponse(clientSocket, HTTP_502_BAD_GATEWAY, "Failed to send request");
        goto cleanup_socket;
    }

    logDebug("Waiting for response headers");

    ssize_t received = recvUntilHeaderEnd(remoteSocket, buffer);
    if (received <= 0)
    {
        logError("Failed to receive response headers");
        sendErrorResponse(clientSocket, HTTP_502_BAD_GATEWAY, "Failed to receive response");
        goto cleanup_socket;
    }

    logDebug("Received response headers");

    const char *responseData = Buffer_asString(buffer);

    if (!isResponse200(responseData))
    {
        logDebug("Response is not 200 OK, forwarding without cache");
        if (sendAll(clientSocket, get_Buffer_data(buffer), get_Buffer_size(buffer)) < 0)
        {
            logError("Failed sendall");
        }
        if (forwardResponse(clientSocket, remoteSocket, buffer) < 0)
        {
            logError("Failed sendall");
        }
        goto cleanup_socket;
    }

    logDebug("Response is 200 OK, starting cache");
    *isCacheable = 1;

    node = CacheNodeT_new();
    entry = CacheEntryT_new();

    if (node == NULL || entry == NULL)
    {
        logError("Failed to create cache structures");
        sendErrorResponse(clientSocket, HTTP_500_INTERNAL_ERROR, "Cache allocation failed");
        goto cleanup_cache;
    }

    entry->url = strdup(url);
    if (entry->url == NULL)
    {
        logError("Failed to duplicate URL");
        sendErrorResponse(clientSocket, HTTP_500_INTERNAL_ERROR, "Memory allocation failed");
        goto cleanup_cache;
    }

    CacheEntryT_appendData(entry, get_Buffer_data(buffer), get_Buffer_size(buffer), InProcess);

    if (startBackgroundUpload(entry, buffer, remoteSocket) != SUCCESS)
    {
        logError("Failed to start background upload");
        sendErrorResponse(clientSocket, HTTP_500_INTERNAL_ERROR, "Failed to start download");
        goto cleanup_cache;
    }

    node->entry = entry;
    return node;

cleanup_cache:
    CacheEntryT_delete(entry);
    CacheNodeT_delete(node);

cleanup_socket:
    if (remoteSocket >= 0)
    {
        close(remoteSocket);
    }
    return NULL;
}

static int handleGet(CacheManagerT *cache,
                     Buffer *buffer,
                     const char *host,
                     int port,
                     int clientSocket,
                     const char *url)
{
    int result = ERROR;

    pthread_mutex_lock(&cache->entriesMutex);

    CacheNodeT *node = CacheManagerT_get_CacheNodeT(cache, url);

    if (node == NULL)
    {
        logDebug("Cache MISS");

        int isCacheable = 0;
        node = startDownload(buffer, host, port, clientSocket, url, &isCacheable);

        if (node == NULL)
        {
            pthread_mutex_unlock(&cache->entriesMutex);
            return ERROR;
        }

        CacheManagerT_put_CacheNodeT(cache, node);
    }
    else
    {
        logDebug("Cache HIT");
    }

    pthread_mutex_unlock(&cache->entriesMutex);

    result = sendFromCache(clientSocket, node);

    if (result == SUCCESS)
    {
        logDebug("Request completed successfully");
    }
    else
    {
        logError("Request failed");
    }

    return result;
}

static int handleOther(Buffer *buffer,
                       const char *host,
                       int port,
                       int clientSocket)
{
    int remoteSocket = -1;
    int result = ERROR;

    logDebug("Handling non-GET request");

    remoteSocket = connectToHost(host, port);
    if (remoteSocket < 0)
    {
        logError("Failed to connect to remote host");
        sendErrorResponse(clientSocket, HTTP_502_BAD_GATEWAY, "Failed to connect");
        return ERROR;
    }

    if (sendAll(remoteSocket, get_Buffer_data(buffer), get_Buffer_size(buffer)) < 0)
    {
        logError("Failed to send request");
        sendErrorResponse(clientSocket, HTTP_502_BAD_GATEWAY, "Failed to send request");
        goto cleanup;
    }

    ssize_t received = recvUntilHeaderEnd(remoteSocket, buffer);
    if (received <= 0)
    {
        logError("Failed to receive response");
        sendErrorResponse(clientSocket, HTTP_502_BAD_GATEWAY, "Failed to receive response");
        goto cleanup;
    }

    if (sendAll(clientSocket, get_Buffer_data(buffer), get_Buffer_size(buffer)) < 0)
    {
        logError("Failed to send response headers");
        goto cleanup;
    }

    if (forwardResponse(clientSocket, remoteSocket, buffer) < 0)
    {
        logError("Failed to forward response body");
        goto cleanup;
    }

    logDebug("Non-GET request finished");
    result = SUCCESS;

cleanup:
    if (remoteSocket >= 0)
    {
        close(remoteSocket);
    }
    return result;
}

static int processRequest(CacheManagerT *cache, Buffer *buffer, int clientSocket)
{
    char method[METHOD_MAX_LEN];
    char url[URL_MAX_LEN];
    char protocol[PROTOCOL_MAX_LEN];
    char host[HOST_MAX_LEN];
    char path[PATH_MAX_LEN];
    int port;

    logDebug("Processing new request");

    if (recvUntilHeaderEnd(clientSocket, buffer) <= 0)
    {
        logError("Failed to receive request");
        sendErrorResponse(clientSocket, HTTP_400_BAD_REQUEST, "Failed to read request");
        return ERROR;
    }

    const char *requestData = Buffer_asString(buffer);

    if (sscanf(requestData, "%15s %2047s %15s", method, url, protocol) != 3)
    {
        logError("Invalid request format");
        sendErrorResponse(clientSocket, HTTP_400_BAD_REQUEST, "Invalid request format");
        return ERROR;
    }

    logDebug("Request parsed successfully");

    if (parseUrl(url, host, path, &port) != SUCCESS)
    {
        logError("Invalid URL in request");
        sendErrorResponse(clientSocket, HTTP_400_BAD_REQUEST, "Invalid URL");
        return ERROR;
    }

    if (isGetRequest(method))
    {
        logDebug("Handling GET request");
        return handleGet(cache, buffer, host, port, clientSocket, url);
    }
    else
    {
        logDebug("Handling non-GET request");
        return handleOther(buffer, host, port, clientSocket);
    }
}

void *handleClientThread(void *args)
{
    ClientContext *ctx = args;
    Buffer *buffer = NULL;

    logDebug("Client thread started");

    buffer = Buffer_create(BUFFER_SIZE);
    if (buffer == NULL)
    {
        logError("Failed to create buffer");
        sendErrorResponse(ctx->clientSocket, HTTP_500_INTERNAL_ERROR, "Memory allocation failed");
        goto cleanup;
    }

    processRequest(ctx->cacheManager, buffer, ctx->clientSocket);

cleanup:
    Buffer_destroy(buffer);
    close(ctx->clientSocket);
    free(ctx);

    pthread_mutex_lock(&clientsMutex);
    activeClients--;
    if (activeClients == 0)
    {
        pthread_cond_signal(&clientsCond);
    }
    pthread_mutex_unlock(&clientsMutex);

    logDebug("Client thread finished");
    return NULL;
}