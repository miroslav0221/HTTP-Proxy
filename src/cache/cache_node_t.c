#include "cache.h"
#include <stdlib.h>
#include <string.h>

CacheNodeT *CacheNodeT_new(void) {
    CacheNodeT *node = malloc(sizeof(CacheNodeT));
    if (node == NULL) {
        return NULL;
    }
    
    memset(node, 0, sizeof(CacheNodeT));
    return node;
}

void CacheNodeT_delete(CacheNodeT *node) {
    if (node == NULL) {
        return;
    }
    
    CacheEntryT_delete(node->entry);
    free(node);
}