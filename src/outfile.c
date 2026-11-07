#include "outfile.h"
#include "common.h"
#include <stdarg.h>
#include <stdio.h>

// Строка out_line, символов, вместе с CRLF и нулём
#define OUT_LINE_CCH 2048

// В конце строки out_line - "\r\n" и завершающий ноль
#define LINE_END_CCH 3

// Текст пишется кусками по столько символов (в UTF-8 - не больше трёх байтов на символ UTF-16)
#define TEXT_CHUNK_CCH           4096
#define UTF8_MAX_BYTES_PER_WCHAR 3

// Старший суррогат пары UTF-16 (D800..DBFF): кусок текста на нём не обрываем
#define HIGH_SURROGATE_FIRST 0xD800
#define HIGH_SURROGATE_LAST  0xDBFF

static const BYTE UTF8_BOM[] = { 0xEF, 0xBB, 0xBF };

static void write_bytes(OUT_FILE *file, const void *bytes, DWORD size)
{
    DWORD written = 0;

    if (file->handle == INVALID_HANDLE_VALUE || size == 0) return;

    if (!WriteFile(file->handle, bytes, size, &written, NULL) || written != size) file->failed = TRUE;
}

BOOL out_open(OUT_FILE *file, const WCHAR *path)
{
    LARGE_INTEGER size;

    file->failed = FALSE;
    file->handle = CreateFileW(path, FILE_APPEND_DATA | FILE_READ_ATTRIBUTES, FILE_SHARE_READ, NULL, OPEN_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, NULL);

    if (file->handle == INVALID_HANDLE_VALUE)
    {
        file->failed = TRUE;
        return FALSE;
    }

    size.QuadPart = 0;

    if (GetFileSizeEx(file->handle, &size) && size.QuadPart == 0) write_bytes(file, UTF8_BOM, sizeof(UTF8_BOM));

    return TRUE;
}

void out_close(OUT_FILE *file)
{
    if (file->handle != INVALID_HANDLE_VALUE) CloseHandle(file->handle);

    file->handle = INVALID_HANDLE_VALUE;
}

static BOOL is_high_surrogate(WCHAR symbol)
{
    return symbol >= HIGH_SURROGATE_FIRST && symbol <= HIGH_SURROGATE_LAST;
}

void out_text(OUT_FILE *file, const WCHAR *text, size_t length)
{
    char utf8[TEXT_CHUNK_CCH * UTF8_MAX_BYTES_PER_WCHAR];
    size_t done = 0;

    while (done < length)
    {
        size_t chunk = length - done;

        if (chunk > TEXT_CHUNK_CCH) chunk = TEXT_CHUNK_CCH;

        // суррогатную пару не делим между кусками
        if (chunk > 1 && done + chunk < length && is_high_surrogate(text[done + chunk - 1])) chunk--;

        int size = WideCharToMultiByte(CP_UTF8, 0, text + done, (int)chunk, utf8, sizeof(utf8), NULL, NULL);

        if (size > 0) write_bytes(file, utf8, (DWORD)size);
        else file->failed = TRUE;

        done += chunk;
    }
}

void out_line(OUT_FILE *file, const WCHAR *format, ...)
{
    WCHAR line[OUT_LINE_CCH];
    va_list args;

    va_start(args, format);
    int length = _vsnwprintf(line, OUT_LINE_CCH - LINE_END_CCH, format, args);
    va_end(args);

    if (length < 0) length = OUT_LINE_CCH - LINE_END_CCH;   // обрезано

    line[length++] = L'\r';
    line[length++] = L'\n';
    line[length] = 0;

    out_text(file, line, (size_t)length);
}

void out_header(OUT_FILE *file, const WCHAR *title)
{
    out_line(file, L"");
    out_line(file, L"[%s]", title);
}
