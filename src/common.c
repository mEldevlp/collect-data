#include "common.h"
#include <stdio.h>
#include <stdarg.h>

int format_text(WCHAR *buffer, int buffer_cch, const WCHAR *format, ...)
{
    va_list args;

    va_start(args, format);
    int length = _vsnwprintf(buffer, buffer_cch - 1, format, args);
    va_end(args);

    // _vsnwprintf не ставит завершающий ноль, если текст занял весь буфер или обрезан
    buffer[buffer_cch - 1] = 0;

    return length;
}

const WCHAR *skip_prefix(const WCHAR *text, const WCHAR *prefix)
{
    size_t prefix_length = wcslen(prefix);

    if (_wcsnicmp(text, prefix, prefix_length) != 0) return NULL;

    return text + prefix_length;
}

static BOOL is_list_separator(WCHAR symbol)
{
    return symbol == L',' || symbol == L';';
}

BOOL next_list_item(const WCHAR **cursor, const WCHAR **start, size_t *length)
{
    const WCHAR *item_start = *cursor;

    if (*item_start == 0) return FALSE;

    const WCHAR *item_end = item_start;

    while (*item_end && !is_list_separator(*item_end)) item_end++;

    *start = item_start;
    *length = (size_t)(item_end - item_start);
    *cursor = *item_end ? item_end + 1 : item_end;

    return TRUE;
}
