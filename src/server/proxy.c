#include "proxy.h"
#include "log.h"

#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#define SERVER_BACKLOG 10

const char *HTTP_400_BAD_REQUEST = "400 Bad Request";
const char *HTTP_500_INTERNAL_ERROR = "500 Internal Server Error";
const char *HTTP_502_BAD_GATEWAY = "502 Bad Gateway";

sig_atomic_t serverShutdown = 0;
int activeClients = 0;
pthread_mutex_t clientsMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t clientsCond = PTHREAD_COND_INITIALIZER;

static void sighandler(int sig)
{
    (void)sig;
    serverShutdown = 1;
    logInfo("Received shutdown signal");
}

static void setupSigHandlers(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);

    sa.sa_handler = SIG_IGN;
    for (int sig = 1; sig < NSIG; sig++)
    {
        if (sig == SIGKILL || sig == SIGSTOP)
        {
            continue;
        }
        sigaction(sig, &sa, NULL);
    }

    sa.sa_handler = sighandler;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    logDebug("Signal handlers configured");
}

static int createServerSocket(int port)
{
    int sock = -1;
    int ret = ERROR;

    sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0)
    {
        logError("Failed to create socket");
        return ERROR;
    }

    int opt = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
    {
        logError("Failed to set socket options");
        goto cleanup;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        logError("Failed to bind socket");
        goto cleanup;
    }

    if (listen(sock, SERVER_BACKLOG) < 0)
    {
        logError("Failed to listen on socket");
        goto cleanup;
    }

    logDebug("Server socket created");
    return sock;

cleanup:
    if (sock >= 0)
    {
        close(sock);
    }
    return ret;
}

static void handleNewClient(int serverSocket, CacheManagerT *cacheManager)
{
    struct sockaddr_in clientAddr;
    socklen_t addrLen = sizeof(clientAddr);
    ClientContext *ctx = NULL;
    pthread_t thread;

    int clientSocket = accept(serverSocket,
                              (struct sockaddr *)&clientAddr,
                              &addrLen);
    if (clientSocket < 0)
    {
        return;
    }

    logDebug("New client connection accepted");

    if (setSocketTimeout(clientSocket, SOCKET_TIMEOUT_SEC) != SUCCESS)
    {
        goto cleanup;
    }

    ctx = malloc(sizeof(ClientContext));
    if (ctx == NULL)
    {
        logError("Failed to allocate client context");
        sendErrorResponse(clientSocket, HTTP_500_INTERNAL_ERROR, "");
        goto cleanup;
    }

    ctx->cacheManager = cacheManager;
    ctx->clientSocket = clientSocket;

    pthread_mutex_lock(&clientsMutex);
    activeClients++;
    pthread_mutex_unlock(&clientsMutex);

    if (pthread_create(&thread, NULL, handleClientThread, ctx) != 0)
    {
        logError("Failed to create client thread");
        sendErrorResponse(clientSocket, HTTP_500_INTERNAL_ERROR, "");

        pthread_mutex_lock(&clientsMutex);
        activeClients--;
        pthread_mutex_unlock(&clientsMutex);

        goto cleanup;
    }

    pthread_detach(thread);
    return;

cleanup:
    free(ctx);
    if (clientSocket >= 0)
    {
        close(clientSocket);
    }
}

static void waitForAllClients(void)
{
    pthread_mutex_lock(&clientsMutex);
    while (activeClients > 0)
    {
        logInfo("Waiting for clients to finish");
        pthread_cond_wait(&clientsCond, &clientsMutex);
    }
    pthread_mutex_unlock(&clientsMutex);
}

void startProxyServer(int port)
{
    int serverSocket = -1;
    CacheManagerT *cacheManager = NULL;

    logInfo("Starting proxy server");

    setupSigHandlers();

    serverSocket = createServerSocket(port);
    if (serverSocket < 0)
    {
        logError("Failed to create server socket");
        return;
    }

    cacheManager = CacheManagerT_new();
    if (cacheManager == NULL)
    {
        logError("Failed to create cache manager");
        goto cleanup;
    }

    logInfo("Server ready, waiting for connections");

    while (!serverShutdown)
    {
        handleNewClient(serverSocket, cacheManager);

        if (serverShutdown)
        {
            logInfo("Shutdown requested");
            break;
        }
    }

cleanup:
    logInfo("Shutting down server");

    if (serverSocket >= 0)
    {
        close(serverSocket);
    }

    waitForAllClients();

    if (cacheManager != NULL)
    {
        CacheManagerT_delete(cacheManager);
    }

    logInfo("Server stopped");
}