#include "buffer.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>

Buffer *Buffer_create(size_t capacity)
{
    Buffer *buf = malloc(sizeof(Buffer));
    if (buf == NULL)
    {
        logError("Failed to allocate buffer structure");
        return NULL;
    }

    buf->data = malloc(capacity);
    if (buf->data == NULL)
    {
        logError("Failed to allocate buffer data");
        free(buf);
        return NULL;
    }

    buf->size = 0;
    buf->capacity = capacity;
    return buf;
}

int Buffer_reserve(Buffer *buffer, size_t minCapacity)
{
    if (buffer->capacity >= minCapacity)
    {
        return 0;
    }

    size_t newCapacity = buffer->capacity;
    while (newCapacity < minCapacity)
    {
        newCapacity *= BUFFER_FACTOR;
    }

    char *newData = realloc(buffer->data, newCapacity);
    if (newData == NULL)
    {
        return -1;
    }

    buffer->data = newData;
    buffer->capacity = newCapacity;
    return 0;
}

void Buffer_destroy(Buffer *buffer)
{
    if (buffer == NULL)
    {
        return;
    }
    free(buffer->data);
    free(buffer);
}

void Buffer_clear(Buffer *buffer)
{
    buffer->size = 0;
}

int Buffer_append(Buffer *buffer, const void *data, size_t len)
{
    if (len == 0)
    {
        return 0;
    }

    size_t required = buffer->size + len;
    if (Buffer_reserve(buffer, required) != 0)
    {
        return -1;
    }

    memcpy(buffer->data + buffer->size, data, len);
    buffer->size += len;
    return 0;
}

int Buffer_appendString(Buffer *buffer, const char *string)
{
    if (string == NULL)
    {
        return 0;
    }

    size_t len = strlen(string);

    size_t required = buffer->size + len + 1;
    if (Buffer_reserve(buffer, required) != 0)
    {
        return -1;
    }

    memcpy(buffer->data + buffer->size, string, len + 1);
    buffer->size += len + 1;
    return 0;
}

const char *get_Buffer_data(const Buffer *buffer)
{
    return buffer->data;
}

size_t get_Buffer_size(const Buffer *buffer)
{
    return buffer->size;
}
size_t get_Buffer_capacity(const Buffer *buffer)
{
    return buffer->capacity;
}

char *Buffer_writePtr(Buffer *buffer)
{
    return buffer->data + buffer->size;
}

size_t Buffer_available(const Buffer *buffer)
{
    return buffer->capacity - buffer->size;
}

void Buffer_advanceSize(Buffer *buffer, size_t count)
{
    buffer->size += count;
    if (buffer->size > buffer->capacity)
    {
        buffer->size = buffer->capacity;
    }
}

const char *Buffer_asString(Buffer *buffer)
{
    if (buffer->size >= buffer->capacity)
    {
        if (Buffer_reserve(buffer, buffer->size + 1) != 0)
        {
            return NULL;
        }
    }

    buffer->data[buffer->size] = '\0';
    return buffer->data;
}