#include "text.h"
#include "common.h"
#include <limits.h>
#include <string.h>

// Метки порядка байтов в начале вывода
#define UTF16_BOM_SIZE   2
#define UTF16_BOM_FIRST  0xFF
#define UTF16_BOM_SECOND 0xFE
#define UTF8_BOM_SIZE    3

static const BYTE UTF8_BOM[UTF8_BOM_SIZE] = { 0xEF, 0xBB, 0xBF };

// Символ UTF-16 из двух байтов (младший первым): второй байт - старшие 8 бит
#define BITS_PER_BYTE 8

// UTF-8: байты меньше 0x80 - ASCII, байт продолжения - 10xxxxxx
#define ASCII_LIMIT            0x80
#define UTF8_CONTINUATION_MASK 0xC0
#define UTF8_CONTINUATION_TAG  0x80

// Первые байты последовательностей: из двух байтов - C2..DF, из трёх - E0..EF, из четырёх - F0..F4
// (C0, C1 и F5..FF в UTF-8 не встречаются)
#define UTF8_LEAD2_FIRST 0xC2
#define UTF8_LEAD2_LAST  0xDF
#define UTF8_LEAD3_FIRST 0xE0
#define UTF8_LEAD3_LAST  0xEF
#define UTF8_LEAD4_FIRST 0xF0
#define UTF8_LEAD4_LAST  0xF4

// Сколько байтов продолжения идёт за первым байтом
#define UTF8_TAIL_OF_TWO   1
#define UTF8_TAIL_OF_THREE 2
#define UTF8_TAIL_OF_FOUR  3

// Ограничения второго байта: после E0 - не меньше A0 (иначе запись длиннее нужной), после ED - не больше 9F
// (иначе суррогат), после F0 - не меньше 90, после F4 - не больше 8F (иначе больше U+10FFFF)
#define UTF8_LEAD_SURROGATES  0xED
#define UTF8_E0_SECOND_MIN    0xA0
#define UTF8_ED_SECOND_MAX    0x9F
#define UTF8_F0_SECOND_MIN    0x90
#define UTF8_F4_SECOND_MAX    0x8F

// Оценка строки для выбора кодовой страницы - как Get-Score в collect_audit.cmd: буквы в словах из двух и более
// русских букв (А..я) минус прочие символы вне ASCII. Ё и ё словом не считаются (в 1251, прочитанной как 866,
// их много), но и не штрафуются, как неразрывный пробел, кавычки-ёлочки, тире и знак номера.
#define CYRILLIC_FIRST        0x0410    // А
#define CYRILLIC_LAST         0x044F    // я
#define CYRILLIC_CAPITAL_IO   0x0401    // Ё
#define CYRILLIC_SMALL_IO     0x0451    // ё
#define NO_BREAK_SPACE        0x00A0
#define LEFT_GUILLEMET        0x00AB
#define RIGHT_GUILLEMET       0x00BB
#define EN_DASH               0x2013
#define EM_DASH               0x2014
#define NUMERO_SIGN           0x2116
#define SCORE_MIN_WORD_LETTERS 2

// Запас в тексте сверх исходных символов: CRLF в конце и завершающий ноль
#define TEXT_TAIL_CCH 3

typedef struct
{
    WCHAR *data;
    size_t length;
    size_t capacity;    // символов, вместе с местом под завершающий ноль
    BOOL   pending_cr;  // был CR, а LF за ним ещё не пришёл
} TEXT_WRITER;

static BOOL is_continuation(BYTE value)
{
    return (value & UTF8_CONTINUATION_MASK) == UTF8_CONTINUATION_TAG;
}

// Число байтов продолжения после первого байта lead; -1 - с такого байта последовательность не начинается.
static int utf8_tail_length(BYTE lead)
{
    if (lead >= UTF8_LEAD2_FIRST && lead <= UTF8_LEAD2_LAST) return UTF8_TAIL_OF_TWO;
    if (lead >= UTF8_LEAD3_FIRST && lead <= UTF8_LEAD3_LAST) return UTF8_TAIL_OF_THREE;
    if (lead >= UTF8_LEAD4_FIRST && lead <= UTF8_LEAD4_LAST) return UTF8_TAIL_OF_FOUR;

    return -1;
}

static BOOL second_byte_allowed(BYTE lead, BYTE second)
{
    if (lead == UTF8_LEAD3_FIRST) return second >= UTF8_E0_SECOND_MIN;
    if (lead == UTF8_LEAD_SURROGATES) return second <= UTF8_ED_SECOND_MAX;
    if (lead == UTF8_LEAD4_FIRST) return second >= UTF8_F0_SECOND_MIN;
    if (lead == UTF8_LEAD4_LAST) return second <= UTF8_F4_SECOND_MAX;

    return TRUE;
}

// Правильный ли это UTF-8. *non_ascii - есть ли в нём символы вне ASCII.
static BOOL utf8_valid(const BYTE *bytes, size_t size, BOOL *non_ascii)
{
    size_t position = 0;

    *non_ascii = FALSE;

    while (position < size)
    {
        BYTE lead = bytes[position];

        if (lead < ASCII_LIMIT)
        {
            position++;
            continue;
        }

        int tail = utf8_tail_length(lead);

        if (tail < 0 || size - position <= (size_t)tail) return FALSE;
        if (!second_byte_allowed(lead, bytes[position + 1])) return FALSE;

        for (int i = 1; i <= tail; i++)
        {
            if (!is_continuation(bytes[position + i])) return FALSE;
        }

        *non_ascii = TRUE;
        position += (size_t)tail + 1;
    }

    return TRUE;
}

static BOOL is_basic_cyrillic(WCHAR symbol)
{
    return symbol >= CYRILLIC_FIRST && symbol <= CYRILLIC_LAST;
}

// Символ, который в русском тексте ожидаем и за который оценка не снижается.
static BOOL is_expected_symbol(WCHAR symbol)
{
    return symbol < ASCII_LIMIT || is_basic_cyrillic(symbol) || symbol == CYRILLIC_CAPITAL_IO ||
           symbol == CYRILLIC_SMALL_IO || symbol == NO_BREAK_SPACE || symbol == LEFT_GUILLEMET ||
           symbol == RIGHT_GUILLEMET || symbol == EN_DASH || symbol == EM_DASH || symbol == NUMERO_SIGN;
}

static int line_score(const WCHAR *text, int length)
{
    int score = 0;
    int letters = 0;

    for (int i = 0; i < length; i++)
    {
        WCHAR symbol = text[i];

        if (is_basic_cyrillic(symbol))
        {
            letters++;
            continue;
        }

        if (letters >= SCORE_MIN_WORD_LETTERS) score += letters;

        letters = 0;

        if (!is_expected_symbol(symbol)) score--;
    }

    if (letters >= SCORE_MIN_WORD_LETTERS) score += letters;

    return score;
}

static BOOL writer_init(TEXT_WRITER *writer, size_t capacity, BUFFER *text)
{
    ZeroMemory(writer, sizeof(*writer));

    if (buffer_init_array(text, capacity, sizeof(WCHAR)) != BUFFER_OK) return FALSE;

    writer->data = text->data;
    writer->capacity = capacity;

    return TRUE;
}

static void writer_put(TEXT_WRITER *writer, WCHAR symbol)
{
    if (writer->length + 1 < writer->capacity) writer->data[writer->length++] = symbol;
}

static void writer_line_break(TEXT_WRITER *writer)
{
    writer_put(writer, L'\r');
    writer_put(writer, L'\n');
}

// Символ в текст: NUL выбрасывается, LF, CR+LF, CR+CR+LF и одиночный CR - один перевод строки CRLF.
static void writer_add(TEXT_WRITER *writer, WCHAR symbol)
{
    if (symbol == 0) return;

    if (symbol == L'\r')
    {
        writer->pending_cr = TRUE;
        return;
    }

    if (symbol == L'\n')
    {
        writer_line_break(writer);
        writer->pending_cr = FALSE;
        return;
    }

    if (writer->pending_cr)
    {
        writer_line_break(writer);
        writer->pending_cr = FALSE;
    }

    writer_put(writer, symbol);
}

static void writer_add_text(TEXT_WRITER *writer, const WCHAR *text, int length)
{
    for (int i = 0; i < length; i++) writer_add(writer, text[i]);
}

// Конец текста: CRLF после последней строки, завершающий ноль, размер буфера - по длине текста.
static void writer_finish(TEXT_WRITER *writer, BUFFER *text)
{
    if (writer->pending_cr) writer_line_break(writer);

    if (writer->length > 0 && writer->data[writer->length - 1] != L'\n') writer_line_break(writer);

    writer->data[writer->length] = 0;
    text->size = (writer->length + 1) * sizeof(WCHAR);
}

// Сколько в тексте байтов CR и LF: каждый может стать двумя символами CRLF.
static size_t count_line_ends(const BYTE *bytes, size_t size)
{
    size_t count = 0;

    for (size_t i = 0; i < size; i++)
    {
        if (bytes[i] == '\r' || bytes[i] == '\n') count++;
    }

    return count;
}

static int decode_utf16(const BYTE *bytes, size_t size, BUFFER *text)
{
    size_t units = size / sizeof(WCHAR);
    size_t line_ends = 0;
    TEXT_WRITER writer;

    for (size_t i = 0; i < units; i++)
    {
        WCHAR symbol = (WCHAR)(bytes[i * sizeof(WCHAR)] | (bytes[i * sizeof(WCHAR) + 1] << BITS_PER_BYTE));

        if (symbol == L'\r' || symbol == L'\n') line_ends++;
    }

    if (!writer_init(&writer, units + line_ends + TEXT_TAIL_CCH, text)) return BUFFER_ERROR;

    for (size_t i = 0; i < units; i++)
    {
        writer_add(&writer, (WCHAR)(bytes[i * sizeof(WCHAR)] | (bytes[i * sizeof(WCHAR) + 1] << BITS_PER_BYTE)));
    }

    writer_finish(&writer, text);

    return BUFFER_OK;
}

// Длина самой длинной строки (байтов, вместе с LF).
static size_t longest_line(const BYTE *bytes, size_t size)
{
    size_t longest = 0;
    size_t start = 0;

    for (size_t i = 0; i < size; i++)
    {
        if (bytes[i] != '\n') continue;

        if (i + 1 - start > longest) longest = i + 1 - start;

        start = i + 1;
    }

    if (size - start > longest) longest = size - start;

    return longest;
}

// Строка в кодовой странице code_page - в wide (wide_cch символов). Возвращает длину, 0 - не перекодировать.
static int decode_with(UINT code_page, const BYTE *line, int length, WCHAR *wide, int wide_cch)
{
    return MultiByteToWideChar(code_page, 0, (const char *)line, length, wide, wide_cch);
}

// Одна строка: правильный UTF-8 - как есть, иначе кодовая страница, в которой строка читается лучше.
static void decode_line(TEXT_WRITER *writer, const BYTE *line, int length, BOOL all_utf8, UINT primary_code_page,
                        UINT secondary_code_page, WCHAR *first, WCHAR *second, int wide_cch)
{
    BOOL non_ascii = FALSE;

    if (all_utf8 || (utf8_valid(line, (size_t)length, &non_ascii) && non_ascii))
    {
        writer_add_text(writer, first, decode_with(CP_UTF8, line, length, first, wide_cch));
        return;
    }

    int first_length = decode_with(primary_code_page, line, length, first, wide_cch);
    int second_length = 0;

    if (secondary_code_page != primary_code_page)
    {
        second_length = decode_with(secondary_code_page, line, length, second, wide_cch);
    }

    if (second_length > 0 && (first_length == 0 || line_score(second, second_length) > line_score(first, first_length)))
    {
        writer_add_text(writer, second, second_length);
        return;
    }

    writer_add_text(writer, first, first_length);
}

static int decode_lines(const BYTE *bytes, size_t size, UINT primary_code_page, UINT secondary_code_page,
                        BUFFER *text)
{
    BOOL non_ascii = FALSE;
    BOOL all_utf8 = utf8_valid(bytes, size, &non_ascii);
    size_t longest = longest_line(bytes, size);
    BUFFER first = { 0 };
    BUFFER second = { 0 };
    TEXT_WRITER writer;

    // MultiByteToWideChar принимает длину типа int
    if (longest >= INT_MAX) return BUFFER_ERROR;

    if (!writer_init(&writer, size + count_line_ends(bytes, size) + TEXT_TAIL_CCH, text)) return BUFFER_ERROR;

    if (buffer_init_array(&first, longest + 1, sizeof(WCHAR)) != BUFFER_OK ||
        buffer_init_array(&second, longest + 1, sizeof(WCHAR)) != BUFFER_OK)
    {
        buffer_destroy(&first);
        buffer_destroy(&second);
        buffer_destroy(text);
        return BUFFER_ERROR;
    }

    size_t start = 0;

    while (start < size)
    {
        size_t end = start;

        while (end < size && bytes[end] != '\n') end++;

        if (end < size) end++;

        decode_line(&writer, bytes + start, (int)(end - start), all_utf8, primary_code_page, secondary_code_page,
                    first.data, second.data, (int)(longest + 1));
        start = end;
    }

    buffer_destroy(&first);
    buffer_destroy(&second);
    writer_finish(&writer, text);

    return BUFFER_OK;
}

int text_decode(const BYTE *bytes, size_t size, UINT primary_code_page, UINT secondary_code_page, BUFFER *text)
{
    buffer_init(text, 0);

    if (size >= UTF16_BOM_SIZE && bytes[0] == UTF16_BOM_FIRST && bytes[1] == UTF16_BOM_SECOND)
    {
        return decode_utf16(bytes + UTF16_BOM_SIZE, size - UTF16_BOM_SIZE, text);
    }

    if (size >= UTF8_BOM_SIZE && memcmp(bytes, UTF8_BOM, UTF8_BOM_SIZE) == 0)
    {
        return decode_lines(bytes + UTF8_BOM_SIZE, size - UTF8_BOM_SIZE, primary_code_page, secondary_code_page,
                            text);
    }

    return decode_lines(bytes, size, primary_code_page, secondary_code_page, text);
}

size_t text_length(const BUFFER *text)
{
    if (!text->data || text->size < sizeof(WCHAR)) return 0;

    return text->size / sizeof(WCHAR) - 1;
}
