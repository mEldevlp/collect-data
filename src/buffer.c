#include "buffer.h"
#include <stdint.h>
#include <stdlib.h>

int buffer_init(BUFFER *buffer, size_t size)
{
    if (buffer == NULL) return BUFFER_ERROR;

    buffer->data = NULL;
    buffer->size = 0;

    if (size == 0) return BUFFER_OK;

    buffer->data = malloc(size);

    if (buffer->data == NULL) return BUFFER_ERROR;

    buffer->size = size;

    return BUFFER_OK;
}

int buffer_init_array(BUFFER *buffer, size_t count, size_t element_size)
{
    if (element_size != 0 && count > SIZE_MAX / element_size)
    {
        buffer_init(buffer, 0);     // пустой буфер: buffer_destroy после ошибки безопасен
        return BUFFER_ERROR;
    }

    return buffer_init(buffer, count * element_size);
}

void buffer_destroy(BUFFER *buffer)
{
    if (buffer == NULL) return;

    free(buffer->data);

    buffer->data = NULL;
    buffer->size = 0;
}
