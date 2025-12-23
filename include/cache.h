#ifndef CACHE_H
#define CACHE_H

#include <pthread.h>
#include <sys/time.h>
#include <stddef.h>

typedef struct CacheEntry CacheEntryT;
typedef struct CacheNode CacheNodeT;
typedef struct CacheManager CacheManagerT;
typedef struct CacheEntryChunk CacheEntryChunkT;

typedef enum CacheStatus
{
    InProcess,
    Success,
    Failed
} CacheStatusT;

struct CacheEntryChunk
{
    char *data;
    size_t curDataSize;
    size_t maxDataSize;
    CacheEntryChunkT *next;
};

struct CacheEntry
{
    char *url;
    CacheEntryChunkT *dataChunks;
    CacheEntryChunkT *lastChunk;
    size_t downloadedSize;
    CacheStatusT status;
    pthread_mutex_t dataMutex;
    pthread_cond_t dataCond;
};

struct CacheNode
{
    CacheEntryT *entry;
    CacheNodeT *next;
};

struct CacheManager
{
    pthread_mutex_t entriesMutex;
    CacheNodeT *nodes;
    CacheNodeT *lastNode;
};

CacheEntryChunkT *CacheEntryChunkT_new(size_t dataSize);
void CacheEntryChunkT_delete(CacheEntryChunkT *chunk);

CacheEntryT *CacheEntryT_new(void);
void CacheEntryT_delete(CacheEntryT *entry);

void CacheEntryT_updateStatus(CacheEntryT *entry, CacheStatusT status);
CacheEntryChunkT *CacheEntryT_appendData(CacheEntryT *entry, const char *data,
                                         size_t dataSize, CacheStatusT status);

CacheNodeT *CacheNodeT_new(void);
void CacheNodeT_delete(CacheNodeT *node);

CacheManagerT *CacheManagerT_new(void);
void CacheManagerT_delete(CacheManagerT *manager);
CacheNodeT *CacheManagerT_get_CacheNodeT(CacheManagerT *cache, const char *url);
void CacheManagerT_put_CacheNodeT(CacheManagerT *cache, CacheNodeT *node);

#endif
