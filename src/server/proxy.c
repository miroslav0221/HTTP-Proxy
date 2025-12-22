#include "proxy.h"

#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#define SERVER_BACKLOG 10

const char *HTTP_400_BAD_REQUEST    = "400 Bad Request";
const char *HTTP_500_INTERNAL_ERROR = "500 Internal Server Error";
const char *HTTP_502_BAD_GATEWAY    = "502 Bad Gateway";



static int createServerSocket(int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return ERROR;
    }

    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        return ERROR;
    }

    if (listen(sock, SERVER_BACKLOG) < 0) {
        close(sock);
        return ERROR;
    }

    return sock;
}

void startProxyServer(int port) {

    int serverSocket = createServerSocket(port);
    if (serverSocket < 0) {
        return;
    }

    CacheManagerT *cacheManager = CacheManagerT_new();
    if (cacheManager == NULL) {
        close(serverSocket);
        return;
    }

    while (1) {
        struct sockaddr_in clientAddr;
        socklen_t addrLen = sizeof(clientAddr);

        int clientSocket = accept(serverSocket, 
                                  (struct sockaddr *)&clientAddr, 
                                  &addrLen);
        if (clientSocket < 0) {
            continue;
        }

        ClientContext *ctx = malloc(sizeof(ClientContext));
        if (ctx == NULL) {
            sendErrorResponse(clientSocket, HTTP_500_INTERNAL_ERROR, "");
            close(clientSocket);
            continue;
        }
        
        ctx->cacheManager = cacheManager;
        ctx->clientSocket = clientSocket;

        pthread_t thread;
        if (pthread_create(&thread, NULL, handleClientThread, ctx) != 0) {
            sendErrorResponse(clientSocket, HTTP_500_INTERNAL_ERROR, "");
            free(ctx);
            close(clientSocket);
            continue;
        }
        pthread_detach(thread);
    }
}