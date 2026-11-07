#include "log.h"
#include "common.h"
#include <stdio.h>
#include <stdarg.h>

// Строка журнала, символов, вместе с временем и переводом строки. Длиннее - обрезается.
#define LOG_LINE_CCH 2048

// Время в начале строки - "07.11.2026 10:15:00.123  " (25 символов); буфер с запасом.
#define LOG_TIME_PREFIX_CCH 64

// В конце строки журнала - "\r\n" и завершающий ноль.
#define LOG_LINE_END_CCH 3

// Сколько байт может занять один символ UTF-16 после перекодирования: в UTF-8 - не больше 3
// (суррогатная пара - это 2 символа UTF-16 и 4 байта), в OEM-кодировке - не больше 2
// (двухбайтовые кодовые страницы). По этим числам считаются буферы перекодирования строки.
#define UTF8_MAX_BYTES_PER_WCHAR 3
#define OEM_MAX_BYTES_PER_WCHAR  2

// log_errtext: после "ошибка <код>" нужно место под ": ", хотя бы один символ описания
// и завершающий ноль.
#define ERRTEXT_SEPARATOR   L": "
#define ERRTEXT_RESERVE_CCH (LITERAL_LENGTH(ERRTEXT_SEPARATOR) + 2)

static const BYTE UTF8_BOM[] = { 0xEF, 0xBB, 0xBF };

static HANDLE           g_log_file = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_log_lock;
static BOOL             g_log_lock_ready = FALSE;

static void ensure_log_lock(void)
{
    if (g_log_lock_ready) return;

    InitializeCriticalSection(&g_log_lock);
    g_log_lock_ready = TRUE;
}

BOOL log_open(const WCHAR *dir, const WCHAR *file_name)
{
    WCHAR path[PATH_BUFFER_CCH];
    LARGE_INTEGER size;

    log_close();
    format_text(path, PATH_BUFFER_CCH, L"%s\\%s", dir, file_name);

    HANDLE file = CreateFileW(path, FILE_APPEND_DATA | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

    if (file == INVALID_HANDLE_VALUE) return FALSE;

    EnterCriticalSection(&g_log_lock);
    g_log_file = file;
    LeaveCriticalSection(&g_log_lock);

    // новый (пустой) файл начинаем с BOM, при дописывании в старый - не повторяем
    size.QuadPart = 0;

    if (GetFileSizeEx(g_log_file, &size) && size.QuadPart == 0)
    {
        DWORD written;

        WriteFile(g_log_file, UTF8_BOM, sizeof(UTF8_BOM), &written, NULL);
    }

    return TRUE;
}

void log_close(void)
{
    ensure_log_lock();
    EnterCriticalSection(&g_log_lock);

    if (g_log_file != INVALID_HANDLE_VALUE)
    {
        CloseHandle(g_log_file);
        g_log_file = INVALID_HANDLE_VALUE;
    }

    LeaveCriticalSection(&g_log_lock);
}

// Вывод в консоль: WriteConsoleW, если это консоль, иначе в OEM-кодировке
// (вывод перенаправлен в файл или канал из cmd).
static void write_console(const WCHAR *text, int length)
{
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode, written;

    if (output == NULL || output == INVALID_HANDLE_VALUE) return;

    if (GetConsoleMode(output, &mode))
    {
        WriteConsoleW(output, text, (DWORD)length, &written, NULL);
        return;
    }

    char oem_text[LOG_LINE_CCH * OEM_MAX_BYTES_PER_WCHAR];
    int oem_length = WideCharToMultiByte(CP_OEMCP, 0, text, length, oem_text, sizeof(oem_text), NULL, NULL);

    if (oem_length > 0) WriteFile(output, oem_text, (DWORD)oem_length, &written, NULL);
}

static void write_log_file(const WCHAR *text, int length)
{
    char utf8_text[LOG_LINE_CCH * UTF8_MAX_BYTES_PER_WCHAR];
    DWORD written;

    if (g_log_file == INVALID_HANDLE_VALUE) return;

    int utf8_length = WideCharToMultiByte(CP_UTF8, 0, text, length, utf8_text, sizeof(utf8_text), NULL, NULL);

    if (utf8_length > 0) WriteFile(g_log_file, utf8_text, (DWORD)utf8_length, &written, NULL);
}

static void log_vprint(BOOL with_time, const WCHAR *format, va_list args)
{
    WCHAR line[LOG_LINE_CCH];
    int length = 0;

    if (with_time)
    {
        SYSTEMTIME now;

        GetLocalTime(&now);
        length = _snwprintf(line, LOG_TIME_PREFIX_CCH, L"%02u.%02u.%04u %02u:%02u:%02u.%03u  ",
                            now.wDay, now.wMonth, now.wYear,
                            now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);

        if (length < 0) length = 0;
    }

    int text_capacity = LOG_LINE_CCH - length - LOG_LINE_END_CCH;
    int text_length = _vsnwprintf(line + length, text_capacity, format, args);

    if (text_length < 0) text_length = text_capacity;   // текст обрезан

    length += text_length;
    line[length++] = L'\r';
    line[length++] = L'\n';
    line[length] = 0;

    ensure_log_lock();
    EnterCriticalSection(&g_log_lock);
    write_log_file(line, length);
    write_console(line, length);
    LeaveCriticalSection(&g_log_lock);
}

void log_msg(const WCHAR *format, ...)
{
    va_list args;

    va_start(args, format);
    log_vprint(TRUE, format, args);
    va_end(args);
}

void log_raw(const WCHAR *format, ...)
{
    va_list args;

    va_start(args, format);
    log_vprint(FALSE, format, args);
    va_end(args);
}

static BOOL is_trailing_space(WCHAR symbol)
{
    return symbol == L'\r' || symbol == L'\n' || symbol == L' ';
}

const WCHAR *log_errtext(DWORD error_code, WCHAR *buffer, int buffer_cch)
{
    int length = format_text(buffer, buffer_cch, L"ошибка %lu", error_code);

    if (length <= 0 || length >= buffer_cch - (int)ERRTEXT_RESERVE_CCH) return buffer;

    WCHAR *separator = buffer + length;
    WCHAR *description = separator + LITERAL_LENGTH(ERRTEXT_SEPARATOR);

    lstrcpyW(separator, ERRTEXT_SEPARATOR);

    DWORD description_length = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL,
                                              error_code, 0, description, (DWORD)(buffer_cch - (description - buffer)),
                                              NULL);

    if (description_length == 0)
    {
        *separator = 0;     // система не знает такого кода - остаётся только "ошибка <код>"
        return buffer;
    }

    // FormatMessage заканчивает описание переводом строки - убираем его
    while (description_length > 0 && is_trailing_space(description[description_length - 1]))
    {
        description[--description_length] = 0;
    }

    return buffer;
}
