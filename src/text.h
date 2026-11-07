// Перекодирование вывода консольных программ в текст UTF-16 (сбор данных).
#ifndef SCREENS_TEXT_H
#define SCREENS_TEXT_H

#include <windows.h>
#include "buffer.h"

// Вывод программы (size байт) - в текст UTF-16 с переводами строк CRLF и завершающим нулём: text->data - WCHAR *,
// text->size - размер в байтах вместе с нулём. Кодировка - как у :convert_utf8 в collect_audit.cmd:
// BOM UTF-16 или UTF-8 - по нему; иначе весь текст UTF-8, если он правильный; иначе построчно: правильная
// строка UTF-8 - как есть, остальные - в кодовой странице primary_code_page или secondary_code_page, в которой
// строка читается лучше (часть программ пишет в OEM, часть - в ANSI, netsh в Windows 11 - в UTF-8).
// Символы NUL выбрасываются, одиночный CR - перевод строки, в конце непустого текста - CRLF.
// Текст должен быть пустым буфером. BUFFER_OK или BUFFER_ERROR (нет памяти; text пустой).
int text_decode(const BYTE *bytes, size_t size, UINT primary_code_page, UINT secondary_code_page, BUFFER *text);

// Длина текста из text_decode, символов, без завершающего нуля (пустой буфер - 0).
size_t text_length(const BUFFER *text);

#endif
