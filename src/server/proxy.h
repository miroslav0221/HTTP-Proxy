#ifndef PROXY_H
#define PROXY_H

#include <pthread.h>
#include <stddef.h>
#include "../cache/cache.h"
#include <sys/types.h>

#define BUFFER_SIZE      16384
#define HOST_MAX_LEN     1024
#define PATH_MAX_LEN     2048
#define MAX_HEADER_SIZE  8192

#define SUCCESS          0
#define ERROR           (-1)

extern const char *HTTP_400_BAD_REQUEST;
extern const char *HTTP_500_INTERNAL_ERROR;
extern const char *HTTP_502_BAD_GATEWAY;

typedef struct Buffer {
    char  *data;
    size_t size;
    size_t capacity;
} Buffer;

typedef struct ClientContext {
    CacheManagerT *cacheManager;
    int            clientSocket;
} ClientContext;

typedef struct FileUploadContext {
    CacheEntryT *entry;
    Buffer      *buffer;
    int          remoteSocket;
} FileUploadContext;

typedef struct HttpResponseInfo {
    int  statusCode;
    long contentLength;  
    int  isChunked;
    int  headerLength;   
} HttpResponseInfo;

Buffer *Buffer_create(size_t capacity);
void    Buffer_destroy(Buffer *buffer);
void    Buffer_clear(Buffer *buffer);

int parseUrl(const char *url, char *host, char *path, int *port);
int parseResponseHeaders(const char *data, size_t len, HttpResponseInfo *info);
int findHeaderEnd(const char *data, size_t len);

int     connectToHost(const char *host, int port);
ssize_t sendAll(int socket, const char *data, size_t size);
ssize_t recvExact(int socket, char *buffer, size_t size);
ssize_t recvUntilHeaderEnd(int socket, Buffer *buffer);

void sendErrorResponse(int socket, const char *status, const char *message);
int  isGetRequest(const char *method);

void  startProxyServer(int port);
void *handleClientThread(void *args);

int   startBackgroundUpload(CacheEntryT *entry, Buffer *buffer, 
                            int remoteSocket);
void *fileUploadThread(void *args);

#endif