#include "proxy.h"
#include "log.h"
#include "buffer.h"

#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>

#define DOWNLOAD_TIMEOUT_SEC 30

static ssize_t recvWithTimeoutUpload(int socket, char *buffer, size_t size)
{
    if (waitForReadable(socket, DOWNLOAD_TIMEOUT_SEC) != SUCCESS)
    {
        if (errno == ETIMEDOUT)
        {
            logError("Download receive timed out");
        }
        return ERROR;
    }

    ssize_t n = recv(socket, buffer, size, 0);

    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
    {
        logError("Download receive failed");
        return ERROR;
    }

    return n;
}

static ssize_t recvToBufferUpload(int socket, Buffer *buffer)
{
    char *ptr = Buffer_writePtr(buffer);
    size_t available = Buffer_available(buffer);

    if (available == 0)
    {
        logError("Buffer is full");
        return ERROR;
    }

    ssize_t n = recvWithTimeoutUpload(socket, ptr, available);

    if (n > 0)
    {
        Buffer_advanceSize(buffer, n);
    }

    return n;
}

void *fileUploadThread(void *args)
{
    FileUploadContext *ctx = args;
    CacheEntryT *entry = ctx->entry;
    Buffer *buffer = ctx->buffer;
    int remoteSocket = ctx->remoteSocket;
    CacheStatusT finalStatus = Success;

    logDebug("File upload thread started");

    while (1)
    {
        Buffer_clear(buffer);

        ssize_t received = recvToBufferUpload(remoteSocket, buffer);

        if (received < 0)
        {
            logError("Failed to receive data from remote");
            finalStatus = Failed;
            break;
        }

        if (received == 0)
        {
            logDebug("Remote connection closed");
            break;
        }

        CacheEntryChunkT *chunk = CacheEntryT_appendData(
            entry, get_Buffer_data(buffer), get_Buffer_size(buffer), InProcess
        );

        if (chunk == NULL)
        {
            logError("Failed to append data to cache");
            finalStatus = Failed;
            break;
        }
    }

    CacheEntryT_updateStatus(entry, finalStatus);

    if (finalStatus == Success)
    {
        logInfo("File upload completed successfully");
    }
    else
    {
        logError("File upload failed");
    }

    close(remoteSocket);
    Buffer_destroy(buffer);
    free(ctx);
    return NULL;
}

int startBackgroundUpload(CacheEntryT *entry,
                          Buffer *requestBuffer,
                          int remoteSocket)
{
    FileUploadContext *ctx = NULL;
    Buffer *uploadBuffer = NULL;
    pthread_t thread;

    logDebug("Starting background upload");

    ctx = malloc(sizeof(FileUploadContext));
    if (ctx == NULL)
    {
        logError("Failed to allocate upload context");
        goto cleanup;
    }

    uploadBuffer = Buffer_create(get_Buffer_capacity(requestBuffer));
    if (uploadBuffer == NULL)
    {
        logError("Failed to create upload buffer");
        goto cleanup;
    }

    ctx->entry = entry;
    ctx->buffer = uploadBuffer;
    ctx->remoteSocket = remoteSocket;

    if (pthread_create(&thread, NULL, fileUploadThread, ctx) != 0)
    {
        logError("Failed to create upload thread");
        goto cleanup;
    }

    pthread_detach(thread);
    logDebug("Background upload thread created");
    return SUCCESS;

cleanup:
    Buffer_destroy(uploadBuffer);
    free(ctx);
    return ERROR;
}