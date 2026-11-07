// Файлы результатов сбора данных: UTF-8 с BOM, переводы строк CRLF, дописывание.
#ifndef SCREENS_OUTFILE_H
#define SCREENS_OUTFILE_H

#include <windows.h>

typedef struct
{
    HANDLE handle;      // INVALID_HANDLE_VALUE - не открыт
    BOOL   failed;      // была ошибка записи
} OUT_FILE;

// Открыть файл для дописывания (нет - создать; новый файл начинается с BOM). FALSE - не открыть,
// причина в GetLastError; file остаётся закрытым, писать в него можно (записи пропадают).
BOOL out_open(OUT_FILE *file, const WCHAR *path);
void out_close(OUT_FILE *file);

// Строка с переводом строки. Формат как у swprintf; длиннее OUT_LINE_CCH - обрезается.
void out_line(OUT_FILE *file, const WCHAR *format, ...);

// Текст как есть (length символов; переводы строк в нём уже CRLF).
void out_text(OUT_FILE *file, const WCHAR *text, size_t length);

// Заголовок блока, как :hdr в collect_audit.cmd: пустая строка и "[title]". По этим заголовкам
// make_md.ps1 делит файл на блоки.
void out_header(OUT_FILE *file, const WCHAR *title);

#endif
