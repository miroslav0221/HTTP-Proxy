#include "cache.h"
#include <stdlib.h>

CacheEntryChunkT *CacheEntryChunkT_new(size_t dataSize)
{
    CacheEntryChunkT *chunk = malloc(sizeof(CacheEntryChunkT));
    if (chunk == NULL)
    {
        return NULL;
    }

    chunk->data = malloc(dataSize);
    if (chunk->data == NULL)
    {
        free(chunk);
        return NULL;
    }

    chunk->maxDataSize = dataSize;
    chunk->curDataSize = 0;
    chunk->next = NULL;

    return chunk;
}

void CacheEntryChunkT_delete(CacheEntryChunkT *chunk)
{
    if (chunk == NULL)
    {
        return;
    }
    free(chunk->data);
    free(chunk);
}
