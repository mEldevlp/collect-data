// Общие константы и мелкие помощники для всех модулей.
#ifndef SCREENS_COMMON_H
#define SCREENS_COMMON_H

#include <windows.h>

// Версия утилиты: в журналах и файле команд сбора
#define SCREENS_VERSION L"0.2"

// Путь к файлу, символов. MAX_PATH (260) - предел классических функций WinAPI для одного пути,
// а у нас путь - это папка результатов плюс имя файла, поэтому буфер с двойным запасом.
#define PATH_BUFFER_CCH (MAX_PATH * 2)

// Короткий текст: описание ошибки WinAPI (код + текст FormatMessage) или окна для журнала.
#define TEXT_BUFFER_CCH 256

// Сообщение из нескольких частей: что делали + описание ошибки WinAPI.
#define MESSAGE_BUFFER_CCH 512

// Имя класса окна. Все классы, которые мы ищем (#32770, CabinetWClass, tooltips_class32...),
// короче 64 символов; более длинное имя чужого класса обрежется и просто не совпадёт.
#define CLASS_NAME_CCH 64

// Пиксель 32-битного DIB - DWORD вида 0xAARRGGBB. Старший байт у снимка экрана не определён
// (GDI его не заполняет), поэтому пиксели сравниваются и переводятся в RGB только по цвету.
#define PIXEL_RGB_MASK    0x00FFFFFF
#define PIXEL_RED_SHIFT   16
#define PIXEL_GREEN_SHIFT 8

#define BYTES_PER_PIXEL_RGB 3   // PNG без альфа-канала: байты R, G, B

#define MS_PER_SECOND 1000

// Доли в процентах: value * SOMETHING_PERCENT / PERCENT_BASE
#define PERCENT_BASE 100

// Основание системы счисления для wcstol/wcstoul: числа в ключах командной строки - десятичные
#define DECIMAL_BASE 10

// Код выхода, с которым мы завершаем чужой (запущенный нами) процесс через TerminateProcess.
#define TERMINATED_EXIT_CODE 1

// C4996: функция объявлена устаревшей или небезопасной (GetVersionExW, sprintf и т. п.)
#define WARN_DEPRECATED 4996

// Число элементов статического массива.
#define ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))

// Длина строкового литерала без завершающего нуля.
#define LITERAL_LENGTH(literal) (ARRAY_COUNT(literal) - 1)

// swprintf в буфер из buffer_cch символов. Строка всегда завершается нулём, даже если обрезана.
// Возвращает длину строки или -1, если текст не поместился и обрезан.
int format_text(WCHAR *buffer, int buffer_cch, const WCHAR *format, ...);

// Если text начинается с prefix (без учёта регистра) - указатель на остаток после префикса,
// иначе NULL. Для разбора ключей вида "/timeout:300".
const WCHAR *skip_prefix(const WCHAR *text, const WCHAR *prefix);

// Очередной элемент списка "a,b;c": start - начало элемента, возвращает его длину и
// передвигает *cursor на следующий элемент. FALSE - список кончился.
BOOL next_list_item(const WCHAR **cursor, const WCHAR **start, size_t *length);

// Входит ли id в список "a,b,c" (без учёта регистра). Для /only.
BOOL list_contains(const WCHAR *list, const WCHAR *id);

#endif
