// Запуск системной программы с захватом вывода (сбор данных): stdout и stderr - во временный файл,
// который удаляется при закрытии, процесс - в job-объекте (по таймауту, сторожевому таймеру или выходу
// утилиты завершается вместе с дочерними), вывод перекодируется в UTF-16 (text.h).
#ifndef SCREENS_EXEC_H
#define SCREENS_EXEC_H

#include <windows.h>
#include "buffer.h"

// Командная строка для журнала и файла команд, символов
#define EXEC_COMMAND_LINE_CCH 1024

// Больше этого вывод не читается: у самых длинных команд сбора (правила брандмауэра на сервере, кэш DNS)
// вывод - единицы мегабайт
#define EXEC_OUTPUT_MAX_BYTES (32 * 1024 * 1024)

typedef struct
{
    const WCHAR *program;       // имя программы в System32 ("systeminfo.exe") или полный путь
    const WCHAR *arguments;     // аргументы или NULL
    DWORD        timeout_ms;    // сколько ждать завершения
    BOOL         answer_no;     // подать на вход "N": net спрашивает, запустить ли остановленную службу
} EXEC_REQUEST;

typedef struct
{
    BOOL   started;             // процесс запущен
    BOOL   not_found;           // программы нет в этой системе
    BOOL   timed_out;           // не завершилась за timeout_ms и остановлена
    BOOL   truncated;           // вывод длиннее предела, прочитано только начало
    DWORD  error;               // ошибка WinAPI: не запустилась или не прочитан вывод (0 - нет)
    DWORD  exit_code;
    DWORD  elapsed_ms;
    BUFFER text;                // вывод в UTF-16 с CRLF и завершающим нулём; пустой буфер - вывода нет
    WCHAR  command_line[EXEC_COMMAND_LINE_CCH];     // что запущено (для журнала и файла команд)
} EXEC_RESULT;

// Запустить программу и дождаться её. TRUE - программа отработала (с любым кодом выхода) и вывод прочитан.
// result заполняется всегда; освобождать - exec_free.
BOOL exec_run(const EXEC_REQUEST *request, EXEC_RESULT *result);

void exec_free(EXEC_RESULT *result);

// Остановить программу, которая сейчас выполняется (сторожевой таймер, Ctrl+C). Можно звать из другого потока.
void exec_abort(void);

#endif
