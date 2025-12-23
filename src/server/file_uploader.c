#include "proxy.h"
#include "log.h"

#include <stdlib.h>
#include <unistd.h>

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
        ssize_t received = recv(remoteSocket, buffer->data, buffer->capacity, 0);

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

        buffer->size = received;

        CacheEntryChunkT *chunk = CacheEntryT_appendData(
            entry, buffer->data, buffer->size, InProcess);

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
        logDebug("File upload completed successfully");
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

    uploadBuffer = Buffer_create(requestBuffer->capacity);
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