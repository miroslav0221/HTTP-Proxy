#include "proxy.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define METHOD_MAX_LEN   16
#define URL_MAX_LEN      2048
#define PROTOCOL_MAX_LEN 16



static void waitForFirstChunk(CacheNodeT *node) {
    CacheEntryT *entry = node->entry;
    
    pthread_mutex_lock(&entry->dataMutex);
    while (entry->status != Failed && entry->dataChunks == NULL) {
        pthread_cond_wait(&entry->dataCond, &entry->dataMutex);
    }
    pthread_mutex_unlock(&entry->dataMutex);
}

static void waitForMoreData(CacheEntryT *entry, 
                            volatile CacheEntryChunkT *chunk,
                            size_t currentSent) {
    pthread_mutex_lock(&entry->dataMutex);
    while (entry->status == InProcess && 
           chunk->next == NULL && 
           chunk->curDataSize == currentSent) {
        pthread_cond_wait(&entry->dataCond, &entry->dataMutex);
    }
    pthread_mutex_unlock(&entry->dataMutex);
}

static int sendAllChunks(int clientSocket, CacheEntryT *entry) {
    volatile CacheEntryChunkT *chunk = entry->dataChunks;
    
    while (chunk != NULL) {
        size_t sent = 0;
        
        while (1) {
            if (entry->status == Failed) {
                return ERROR;
            }
            
            size_t available = chunk->curDataSize;
            if (sent < available) {
                if (sendAll(clientSocket, chunk->data + sent, available - sent) < 0) {
                    return ERROR;
                }
                sent = available;
            }
            
            if (chunk->next != NULL) {
                break;
            }
            
            if (entry->status != InProcess) {
                break;
            }
            
            waitForMoreData(entry, chunk, sent);
        }
        
        chunk = chunk->next;
    }

    return (entry->status == Failed) ? ERROR : SUCCESS;
}

static int sendFromCache(int clientSocket, CacheNodeT *node) {
    waitForFirstChunk(node);
    
    if (node->entry->dataChunks == NULL) {
        return ERROR;
    }
    
    return sendAllChunks(clientSocket, node->entry);
}


static int forwardResponse(int clientSocket, int remoteSocket, Buffer *buffer) {
    while (1) {
        ssize_t n = recv(remoteSocket, buffer->data, buffer->capacity, 0);
        if (n <= 0) break;
        if (sendAll(clientSocket, buffer->data, n) < 0) {
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
                                  int *isCacheable) {
    *isCacheable = 0;
    
    int remoteSocket = connectToHost(host, port);
    if (remoteSocket < 0) {
        sendErrorResponse(clientSocket, HTTP_502_BAD_GATEWAY, 
                          "Failed to connect");
        return NULL;
    }

    if (sendAll(remoteSocket, buffer->data, buffer->size) < 0) {
        close(remoteSocket);
        return NULL;
    }

    ssize_t received = recvUntilHeaderEnd(remoteSocket, buffer);
    if (received <= 0) {
        close(remoteSocket);
        return NULL;
    }

    if (!isResponse200(buffer->data)) {
        sendAll(clientSocket, buffer->data, buffer->size);
        forwardResponse(clientSocket, remoteSocket, buffer);
        close(remoteSocket);
        return NULL;
    }

    *isCacheable = 1;

    CacheNodeT *node = CacheNodeT_new();
    CacheEntryT *entry = CacheEntryT_new();

    if (node == NULL || entry == NULL) {
        CacheEntryT_delete(entry);
        CacheNodeT_delete(node);
        close(remoteSocket);
        return NULL;
    }

    entry->url = strdup(url);
    if (entry->url == NULL) {
        CacheEntryT_delete(entry);
        CacheNodeT_delete(node);
        close(remoteSocket);
        return NULL;
    }

    CacheEntryT_appendData(entry, buffer->data, buffer->size, InProcess);

    if (startBackgroundUpload(entry, buffer, remoteSocket) != SUCCESS) {
        CacheEntryT_delete(entry);
        CacheNodeT_delete(node);
        close(remoteSocket);
        return NULL;
    }

    node->entry = entry;
    return node;
}


static int handleGet(CacheManagerT *cache,
                      Buffer *buffer,
                      const char *host,
                      int port,
                      int clientSocket,
                      const char *url) {
    pthread_mutex_lock(&cache->entriesMutex);

    CacheNodeT *node = CacheManagerT_get_CacheNodeT(cache, url);

    if (node == NULL) {
        int isCacheable = 0;
        node = startDownload(buffer, host, port, clientSocket, url, &isCacheable);
        
        if (node == NULL) {
            pthread_mutex_unlock(&cache->entriesMutex);
            return isCacheable ? ERROR : SUCCESS;
        }

        CacheManagerT_put_CacheNodeT(cache, node);
    }

    CacheEntryT_acquire(node->entry);
    pthread_mutex_unlock(&cache->entriesMutex);

    int result = sendFromCache(clientSocket, node);
    
    CacheEntryT_release(node->entry);
    return result;
}

static int handleOther(Buffer *buffer,
                        const char *host,
                        int port,
                        int clientSocket) {
    int remoteSocket = connectToHost(host, port);
    if (remoteSocket < 0) {
        sendErrorResponse(clientSocket, HTTP_502_BAD_GATEWAY, "");
        return ERROR;
    }

    sendAll(remoteSocket, buffer->data, buffer->size);
    forwardResponse(clientSocket, remoteSocket, buffer);
    
    close(remoteSocket);
    return SUCCESS;
}


static int processRequest(CacheManagerT *cache, Buffer *buffer, int clientSocket) {
    if (recvUntilHeaderEnd(clientSocket, buffer) <= 0) {
        sendErrorResponse(clientSocket, HTTP_400_BAD_REQUEST, "");
        return ERROR;
    }
    buffer->data[buffer->size] = '\0';

    char method[METHOD_MAX_LEN];
    char url[URL_MAX_LEN];
    char protocol[PROTOCOL_MAX_LEN];
    
    if (sscanf(buffer->data, "%15s %2047s %15s", method, url, protocol) != 3) {
        sendErrorResponse(clientSocket, HTTP_400_BAD_REQUEST, "Invalid request");
        return ERROR;
    }

    char host[HOST_MAX_LEN];
    char path[PATH_MAX_LEN];
    int port;
    
    if (parseUrl(url, host, path, &port) != SUCCESS) {
        sendErrorResponse(clientSocket, HTTP_400_BAD_REQUEST, "Invalid URL");
        return ERROR;
    }

    if (isGetRequest(method)) {
        return handleGet(cache, buffer, host, port, clientSocket, url);
    } else {
        return handleOther(buffer, host, port, clientSocket);
    }
}


void *handleClientThread(void *args) {
    ClientContext *ctx = args;
    
    Buffer *buffer = Buffer_create(BUFFER_SIZE);
    if (buffer != NULL) {
        processRequest(ctx->cacheManager, buffer, ctx->clientSocket);
        Buffer_destroy(buffer);
    } else {
        sendErrorResponse(ctx->clientSocket, HTTP_500_INTERNAL_ERROR, "");
    }

    close(ctx->clientSocket);
    free(ctx);
    return NULL;
}