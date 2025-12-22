#include "proxy.h"

#include <stdlib.h>
#include <unistd.h>

void *fileUploadThread(void *args) {
    FileUploadContext *ctx = args;
    CacheEntryT *entry = ctx->entry;
    Buffer *buffer = ctx->buffer;
    int remoteSocket = ctx->remoteSocket;
    
    CacheStatusT finalStatus = Success;

    while (1) {
        ssize_t received = recv(remoteSocket, buffer->data, buffer->capacity, 0);
        
        if (received < 0) {
            finalStatus = Failed;
            break;
        }
        
        if (received == 0) {
            break;
        }

        buffer->size = received;
        
        CacheEntryChunkT *chunk = CacheEntryT_appendData(
            entry, buffer->data, buffer->size, InProcess
        );
        
        if (chunk == NULL) {
            finalStatus = Failed;
            break;
        }
    }

    CacheEntryT_updateStatus(entry, finalStatus);
    
    close(remoteSocket);
    Buffer_destroy(buffer);
    free(ctx);
    return NULL;
}

int startBackgroundUpload(CacheEntryT *entry, 
                          Buffer *requestBuffer,
                          int remoteSocket) {
    FileUploadContext *ctx = malloc(sizeof(FileUploadContext));
    if (ctx == NULL) {
        return ERROR;
    }

    Buffer *uploadBuffer = Buffer_create(requestBuffer->capacity);
    if (uploadBuffer == NULL) {
        free(ctx);
        return ERROR;
    }

    ctx->entry = entry;
    ctx->buffer = uploadBuffer;
    ctx->remoteSocket = remoteSocket;

    pthread_t thread;
    if (pthread_create(&thread, NULL, fileUploadThread, ctx) != 0) {
        Buffer_destroy(uploadBuffer);
        free(ctx);
        return ERROR;
    }

    pthread_detach(thread);
    return SUCCESS;
}