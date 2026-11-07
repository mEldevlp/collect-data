// Динамический буфер: блок памяти вместе с его размером.
// Пустой буфер (data = NULL) - это и обнулённая структура ({ 0 }), и результат неудачного
// buffer_init; buffer_destroy для него безопасен, поэтому освобождать можно всегда.
#ifndef SCREENS_BUFFER_H
#define SCREENS_BUFFER_H

#include <stddef.h>

#define BUFFER_OK    0
#define BUFFER_ERROR (-1)

typedef struct
{
    void  *data;    // NULL - память не выделена
    size_t size;    // размер data в байтах
} BUFFER;

// Выделить size байт (содержимое не обнулено); size = 0 - пустой буфер без памяти.
// Буфер должен быть пустым: старая память не освобождается.
// BUFFER_OK или BUFFER_ERROR (нет памяти; буфер при этом пустой).
int buffer_init(BUFFER *buffer, size_t size);

// Выделить массив из count элементов по element_size байт.
// BUFFER_ERROR - нет памяти или count * element_size не помещается в size_t.
int buffer_init_array(BUFFER *buffer, size_t count, size_t element_size);

// Освободить память; буфер становится пустым.
void buffer_destroy(BUFFER *buffer);

#endif
