#include "cache.h"
#include <stdlib.h>
#include <string.h>

CacheManagerT *CacheManagerT_new(void) {
    CacheManagerT *manager = malloc(sizeof(CacheManagerT));
    if (manager == NULL) {
        return NULL;
    }
    
    memset(manager, 0, sizeof(CacheManagerT));
    
    if (pthread_mutex_init(&manager->entriesMutex, NULL) != 0) {
        free(manager);
        return NULL;
    }
    
    return manager;
}

void CacheManagerT_delete(CacheManagerT *manager) {
    if (manager == NULL) {
        return;
    }
    
    CacheNodeT *node = manager->nodes;
    while (node != NULL) {
        CacheNodeT *next = node->next;
        CacheNodeT_delete(node);
        node = next;
    }
    
    pthread_mutex_destroy(&manager->entriesMutex);
    free(manager);
}

CacheNodeT *CacheManagerT_get_CacheNodeT(CacheManagerT *cache, const char *url) {
    CacheNodeT *node = cache->nodes;
    
    while (node != NULL) {
        if (node->entry && node->entry->url && strcmp(node->entry->url, url) == 0) {
            return node;
        }
        node = node->next;
    }
    
    return NULL;
}

void CacheManagerT_put_CacheNodeT(CacheManagerT *cache, CacheNodeT *node) {
    if (node == NULL) {
        return;
    }
    
    if (cache->nodes == NULL) {
        cache->nodes = node;
        cache->lastNode = node;
    } else {
        cache->lastNode->next = node;
        cache->lastNode = node;
    }
}