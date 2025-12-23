#include "cache.h"
#include <stdlib.h>
#include <string.h>

#define DEFAULT_CHUNK_SIZE (1024 * 1024)

CacheEntryT *CacheEntryT_new(void)
{
    CacheEntryT *entry = malloc(sizeof(CacheEntryT));
    if (entry == NULL)
        goto fail0;

    memset(entry, 0, sizeof(CacheEntryT));
    entry->status = InProcess;

    if (pthread_mutex_init(&entry->dataMutex, NULL) != 0)
        goto fail1;

    if (pthread_cond_init(&entry->dataCond, NULL) != 0)
        goto fail2;

    return entry;

fail2:
    pthread_mutex_destroy(&entry->dataMutex);

fail1:
    free(entry);

fail0:
    return NULL;
}

void CacheEntryT_delete(CacheEntryT *entry)
{
    if (entry == NULL)
    {
        return;
    }

    CacheEntryChunkT *chunk = entry->dataChunks;
    while (chunk != NULL)
    {
        CacheEntryChunkT *next = chunk->next;
        CacheEntryChunkT_delete((CacheEntryChunkT *)chunk);
        chunk = next;
    }

    free(entry->url);
    pthread_mutex_destroy(&entry->dataMutex);
    pthread_cond_destroy(&entry->dataCond);
    free(entry);
}

void CacheEntryT_updateStatus(CacheEntryT *entry, CacheStatusT status)
{
    if (entry == NULL)
    {
        return;
    }
    pthread_mutex_lock(&entry->dataMutex);
    entry->status = status;
    pthread_cond_broadcast(&entry->dataCond);
    pthread_mutex_unlock(&entry->dataMutex);
}

static void appendChunk(CacheEntryT *entry, CacheEntryChunkT *chunk)
{
    if (entry->dataChunks == NULL)
    {
        entry->dataChunks = chunk;
        entry->lastChunk = chunk;
    }
    else
    {
        entry->lastChunk->next = chunk;
        entry->lastChunk = chunk;
    }
    entry->downloadedSize += chunk->maxDataSize;
}

CacheEntryChunkT *CacheEntryT_appendData(CacheEntryT *entry,
                                         const char *data,
                                         size_t dataSize,
                                         CacheStatusT status)
{
    if (dataSize == 0)
    {
        return (CacheEntryChunkT *)entry->lastChunk;
    }

    pthread_mutex_lock(&entry->dataMutex);

    if (entry->dataChunks == NULL)
    {
        CacheEntryChunkT *chunk = CacheEntryChunkT_new(DEFAULT_CHUNK_SIZE);
        if (chunk == NULL)
        {
            entry->status = Failed;
            pthread_cond_broadcast(&entry->dataCond);
            pthread_mutex_unlock(&entry->dataMutex);
            return NULL;
        }
        appendChunk(entry, chunk);
    }

    size_t copied = 0;
    CacheEntryChunkT *current = entry->lastChunk;

    while (copied < dataSize)
    {
        size_t freeSpace = current->maxDataSize - current->curDataSize;

        if (freeSpace == 0)
        {
            CacheEntryChunkT *newChunk = CacheEntryChunkT_new(DEFAULT_CHUNK_SIZE);
            if (newChunk == NULL)
            {
                entry->status = Failed;
                pthread_cond_broadcast(&entry->dataCond);
                pthread_mutex_unlock(&entry->dataMutex);
                return NULL;
            }
            appendChunk(entry, newChunk);
            current = newChunk;
            freeSpace = current->maxDataSize;
        }

        size_t toCopy = (dataSize - copied < freeSpace) ? (dataSize - copied) : freeSpace;
        memcpy(current->data + current->curDataSize, data + copied, toCopy);
        current->curDataSize += toCopy;
        copied += toCopy;
    }

    entry->status = status;
    pthread_cond_broadcast(&entry->dataCond);
    pthread_mutex_unlock(&entry->dataMutex);

    return (CacheEntryChunkT *)current;
}
