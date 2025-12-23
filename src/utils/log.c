#include "log.h"
#include <stdio.h>
#include <time.h>

static void getTimestamp(char *buffer, size_t size)
{
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    strftime(buffer, size, "%H:%M:%S", tm_info);
}

void logDebug(const char *message)
{
    char timestamp[16];
    getTimestamp(timestamp, sizeof(timestamp));
    printf("[%s] [DEBUG] %s\n", timestamp, message);
}

void logInfo(const char *message)
{
    char timestamp[16];
    getTimestamp(timestamp, sizeof(timestamp));
    printf("[%s] [INFO] %s\n", timestamp, message);
}

void logError(const char *message)
{
    char timestamp[16];
    getTimestamp(timestamp, sizeof(timestamp));
    fprintf(stderr, "[%s] [ERROR] %s\n", timestamp, message);
}