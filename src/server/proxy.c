#include "proxy.h"
#include <stdio.h>
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

volatile sig_atomic_t serverShutdown = 0;
int activeClients = 0;
pthread_mutex_t clientsMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t clientsCond = PTHREAD_COND_INITIALIZER;

void sighandler(int sig) {
    serverShutdown = 1;
}


void setupSigHandlers() {
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    
    for (int sig = 1; sig < NSIG; sig++) {
        if (sig == SIGKILL || sig == SIGSTOP) {
            continue;
        }
        sigaction(sig, &sa, NULL);
    }
    
    sa.sa_handler = sighandler;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

static int createServerSocket(int port) {


    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
        return ERROR;
    }

    int opt = 1;
    int ret = setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)); 
    if (ret < 0) {
        close(sock);
        return ERROR;
    }

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
    setupSigHandlers();

    int serverSocket = createServerSocket(port);
    if (serverSocket < 0) {
        return;
    }

    CacheManagerT *cacheManager = CacheManagerT_new();
    if (cacheManager == NULL) {
        close(serverSocket);
        return;
    }

    while (!serverShutdown) {
        struct sockaddr_in clientAddr;
        socklen_t addrLen = sizeof(clientAddr);

        int clientSocket = accept(serverSocket, 
                                  (struct sockaddr *)&clientAddr, 
                                  &addrLen);
        if (clientSocket < 0) {
            break;
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

        pthread_mutex_lock(&clientsMutex);
        activeClients++;
        pthread_mutex_unlock(&clientsMutex);
    }


    close(serverSocket);
    pthread_mutex_lock(&clientsMutex);
    while (activeClients > 0) {
        pthread_cond_wait(&clientsCond, &clientsMutex);
    }
    pthread_mutex_unlock(&clientsMutex);
    CacheManagerT_delete(cacheManager);

}