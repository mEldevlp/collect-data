// Журнал утилиты: файл в UTF-8 с BOM + дублирование в консоль. Потокобезопасен.
#ifndef SCREENS_LOG_H
#define SCREENS_LOG_H

#include <windows.h>

// Открыть журнал <dir>\_журнал_скриншотов.txt (дописывание). FALSE - не удалось,
// тогда пишем только в консоль.
BOOL log_open(const WCHAR *dir);
void log_close(void);

// Строка журнала с временем. Формат как у swprintf.
void log_msg(const WCHAR *format, ...);

// Строка без времени (для дампов и списков).
void log_raw(const WCHAR *format, ...);

// Текст ошибки WinAPI: "ошибка <код>: <описание системы>". Возвращает buffer.
// В buffer должно помещаться хотя бы "ошибка 4294967295" и описание - лучше TEXT_BUFFER_CCH.
const WCHAR *log_errtext(DWORD error_code, WCHAR *buffer, int buffer_cch);

#endif
