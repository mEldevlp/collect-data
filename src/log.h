// Журнал утилиты: файл в UTF-8 с BOM + дублирование в консоль. Потокобезопасен.
#ifndef SCREENS_LOG_H
#define SCREENS_LOG_H

#include <windows.h>

// Журналы в папке результатов: сбор данных и снимки - в разных файлах. Тексты строк журнала снимков
// разбирает vmtest\make_summary.ps1, журнала сбора - make_md.ps1 (строка "Finished") и make_summary.ps1 (время).
#define LOG_COLLECT_FILE L"_журнал_сбора.txt"
#define LOG_SCREENS_FILE L"_журнал_скриншотов.txt"

// Открыть журнал <dir>\<file_name> (дописывание). FALSE - не удалось, тогда пишем только в консоль.
// Открытый ранее журнал закрывается.
BOOL log_open(const WCHAR *dir, const WCHAR *file_name);
void log_close(void);

// Строка журнала с временем. Формат как у swprintf.
void log_msg(const WCHAR *format, ...);

// Строка без времени (для дампов и списков).
void log_raw(const WCHAR *format, ...);

// Текст ошибки WinAPI: "ошибка <код>: <описание системы>". Возвращает buffer.
// В buffer должно помещаться хотя бы "ошибка 4294967295" и описание - лучше TEXT_BUFFER_CCH.
const WCHAR *log_errtext(DWORD error_code, WCHAR *buffer, int buffer_cch);

#endif
