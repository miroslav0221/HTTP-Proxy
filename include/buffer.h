#ifndef BUFFER_H
#define BUFFER_H

#include <stddef.h>

#define BUFFER_FACTOR 2

typedef struct Buffer
{
    char *data;
    size_t size;
    size_t capacity;
} Buffer;

Buffer *Buffer_create(size_t capacity);

void Buffer_destroy(Buffer *buffer);

void Buffer_clear(Buffer *buffer);

const char *get_Buffer_data(const Buffer *buffer);
size_t get_Buffer_size(const Buffer *buffer);
size_t get_Buffer_capacity(const Buffer *buffer);

char *Buffer_writePtr(Buffer *buffer);
size_t Buffer_available(const Buffer *buffer);
void Buffer_advanceSize(Buffer *buffer, size_t count);

const char *Buffer_asString(Buffer *buffer);

#endif