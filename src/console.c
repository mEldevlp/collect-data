// Окно командной строки: ввод команд "с клавиатуры" через помощника (см. console.h).
#include "console.h"
#include "buffer.h"
#include "common.h"
#include "log.h"
#include "wnd.h"
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

// Ключи командной строки помощника
#define HELPER_SWITCH L"/conhelper:"    // /conhelper:<PID консоли> - всегда первым
#define ARG_SIZE      L"/px:"           // /px:<ширина>x<высота> - размер окна в пикселях
#define ARG_FIT       L"/fit"           // уменьшить высоту окна по содержимому
#define ARG_TIMEOUT   L"/tmo:"          // /tmo:<мс> - лимит на одну команду
#define ARG_OUTPUT    L"/out:"          // /out:<файл> - файл результата
#define ARG_COMMAND   L"/c:"            // /c:<команда> - по одному на каждую команду

#define MAX_COMMANDS 16

// Коды выхода помощника
#define HELPER_EXIT_OK      0
#define HELPER_EXIT_ARGS    10
#define HELPER_EXIT_ATTACH  11
#define HELPER_EXIT_HANDLES 12
#define HELPER_EXIT_TIMEOUT 13
#define HELPER_EXIT_INPUT   14
#define HELPER_EXIT_UNKNOWN ((DWORD)-1)     // код не удалось узнать

// Метки строк файла результата
#define TAG_MESSAGE      L'!'   // сообщение для журнала
#define TAG_CONSOLE_LINE L'|'   // строка текста консоли

#define UTF16_BOM 0xFEFF

// Строка консоли в файле результата - не длиннее 1000 символов (шире консоли не бывает).
// Строка файла результата - метка, текст и "\r\n"; с запасом под сообщения помощника.
#define CONSOLE_MAX_COLUMNS   1000
#define HELPER_LINE_CCH       (CONSOLE_MAX_COLUMNS + 100)
#define HELPER_LINE_EXTRA_CCH 3     // метка + "\r\n"

#define DEFAULT_COMMAND_TIMEOUT_MS 30000

// Ожидание, пока консоль успокоится
#define IDLE_POLL_MS      100
#define PROMPT_MIN_MS     300       // приглашение после запуска cmd
#define PROMPT_QUIET_MS   500
#define PROMPT_TIMEOUT_MS 10000
#define COMMAND_MIN_MS    500       // выполнение одной команды
#define COMMAND_QUIET_MS  800

// Размеры консоли
#define FONT_MIN_WIDTH     4        // символ меньше 4x6 пикселей - размер измерен неверно
#define FONT_MIN_HEIGHT    6
#define MIN_COLUMNS        40
#define MIN_ROWS           10
#define MIN_BUFFER_ROWS    300      // буфер не меньше 300 строк - вертикальная прокрутка, как обычно
#define CONTENT_EXTRA_ROWS 3        // по содержимому: курсор в строке Y - это Y + 1 строк и две пустые

// Ожидание помощника основным процессом
#define HELPER_START_RESERVE_MS 20000   // запуск помощника и ожидание приглашения - сверх времени команд
#define HELPER_POLL_MS          200
#define TERMINATE_WAIT_MS       2000

#define HELPER_OUTPUT_MAX_BYTES (4 * 1024 * 1024)   // больше - файл результата испорчен, не читаем

// Командная строка помощника: путь к exe + путь к файлу результата + ключи с числами + команды
#define HELPER_ARGS_RESERVE_CCH  128
#define COMMAND_ARG_OVERHEAD_CCH 8      // ' "/c:' и '"' вокруг каждой команды, с запасом

// Ввод символа: события "нажата" и "отпущена"
#define KEY_EVENTS_PER_SYMBOL 2

// VkKeyScanW: символа нет на раскладке; в старшем байте ответа бит 0 - нужен Shift
#define VK_SCAN_NOT_FOUND  (-1)
#define VK_SCAN_SHIFT_FLAG 1

#ifndef MAPVK_VK_TO_VSC
#define MAPVK_VK_TO_VSC 0               // MapVirtualKey: код виртуальной клавиши -> скан-код
#endif

// Параметры помощника: основной процесс собирает из них командную строку, помощник разбирает
typedef struct
{
    DWORD        console_process_id;
    int          width_px;              // размер окна консоли (0 - не менять)
    int          height_px;
    BOOL         fit_content;
    DWORD        command_timeout_ms;
    const WCHAR *output_path;           // файл результата
    const WCHAR *commands[MAX_COMMANDS];
    int          command_count;
} HELPER_ARGS;

// Сколько ждать, пока консоль успокоится
typedef struct
{
    DWORD min_ms;                       // не раньше
    DWORD quiet_ms;                     // курсор стоит на месте столько
    DWORD timeout_ms;                   // не дольше
} IDLE_WAIT;

// Размеры консоли: символ (из клиентской части окна - шрифт бывает растровым 8x12 на XP/7,
// Consolas на Win10 и т. д.) и рамка с заголовком без полос прокрутки.
typedef struct
{
    int font_width, font_height;        // символ, пикселей
    int frame_width, frame_height;      // рамка + заголовок, без полос прокрутки
    int columns, rows;                  // окно сейчас, символов
} CONSOLE_METRICS;

// Помощник (отдельный процесс)

static HANDLE g_result_file = INVALID_HANDLE_VALUE;

// Строка в файл результата (UTF-16): tag - TAG_MESSAGE или TAG_CONSOLE_LINE.
static void helper_write_line(WCHAR tag, const WCHAR *format, ...)
{
    WCHAR line[HELPER_LINE_CCH];
    int text_capacity = HELPER_LINE_CCH - HELPER_LINE_EXTRA_CCH;
    va_list args;
    DWORD written;

    if (g_result_file == INVALID_HANDLE_VALUE) return;

    line[0] = tag;

    va_start(args, format);
    int length = _vsnwprintf(line + 1, text_capacity, format, args);
    va_end(args);

    if (length < 0) length = text_capacity;     // текст обрезан

    length += 1;    // метка
    line[length++] = L'\r';
    line[length++] = L'\n';

    WriteFile(g_result_file, line, (DWORD)(length * sizeof(WCHAR)), &written, NULL);
}

// Сколько дочерних процессов у process_id (w32tm, net...). conhost.exe не считается: на Win8+
// cmd сам запускает его для своей консоли, и он живёт всё время.
static int count_children(DWORD process_id)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W entry;
    int children = 0;

    if (snapshot == INVALID_HANDLE_VALUE) return 0;

    entry.dwSize = sizeof(entry);

    for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry))
    {
        if (entry.th32ParentProcessID != process_id || entry.th32ProcessID == process_id) continue;
        if (lstrcmpiW(entry.szExeFile, L"conhost.exe") == 0) continue;

        children++;
    }

    CloseHandle(snapshot);

    return children;
}

// Ждать, пока консоль успокоится: ввод разобран, у cmd нет дочерних процессов, курсор
// стоит quiet_ms (и прошло не меньше min_ms). FALSE - не дождались за timeout_ms.
static BOOL wait_idle(HANDLE input, HANDLE output, DWORD process_id, const IDLE_WAIT *wait, COORD *cursor)
{
    DWORD start_tick = GetTickCount();
    DWORD quiet_since = start_tick;
    COORD last_cursor;

    // положения курсора ещё не видели
    last_cursor.X = -1;
    last_cursor.Y = -1;

    for (;;)
    {
        CONSOLE_SCREEN_BUFFER_INFO buffer_info;
        DWORD pending_events = 0;

        Sleep(IDLE_POLL_MS);

        DWORD now = GetTickCount();

        if (!GetConsoleScreenBufferInfo(output, &buffer_info)) return FALSE;
        if (!GetNumberOfConsoleInputEvents(input, &pending_events)) pending_events = 0;

        COORD position = buffer_info.dwCursorPosition;
        BOOL cursor_moved = position.X != last_cursor.X || position.Y != last_cursor.Y;
        BOOL busy = pending_events > 0 || count_children(process_id) > 0 || cursor_moved;

        last_cursor = position;
        *cursor = position;

        if (busy)
        {
            quiet_since = now;
        }
        else if (now - quiet_since >= wait->quiet_ms && now - start_tick >= wait->min_ms)
        {
            return TRUE;
        }

        if (now - start_tick >= wait->timeout_ms) return FALSE;
    }
}

// Код виртуальной клавиши для символа (как на английской раскладке). Консоль берёт сам
// символ (uChar), код клавиши - для порядка.
static WORD virtual_key_for_symbol(WCHAR symbol, DWORD *control_state)
{
    *control_state = 0;

    // коды клавиш букв - заглавные 'A'..'Z', цифр - '0'..'9'
    if (symbol >= L'a' && symbol <= L'z') return (WORD)(symbol - L'a' + 'A');

    if (symbol >= L'A' && symbol <= L'Z')
    {
        *control_state = SHIFT_PRESSED;
        return (WORD)symbol;
    }

    if (symbol >= L'0' && symbol <= L'9') return (WORD)symbol;
    if (symbol == L' ') return VK_SPACE;
    if (symbol == L'\r') return VK_RETURN;

    SHORT scan = VkKeyScanW(symbol);

    if (scan == VK_SCAN_NOT_FOUND) return 0;
    if (HIBYTE(scan) & VK_SCAN_SHIFT_FLAG) *control_state = SHIFT_PRESSED;

    return LOBYTE(scan);
}

static void fill_key_event(INPUT_RECORD *record, BOOL key_down, WCHAR symbol, WORD virtual_key, DWORD control_state)
{
    KEY_EVENT_RECORD *key_event = &record->Event.KeyEvent;

    ZeroMemory(record, sizeof(*record));
    record->EventType = KEY_EVENT;
    key_event->bKeyDown = key_down;
    key_event->wRepeatCount = 1;
    key_event->wVirtualKeyCode = virtual_key;
    key_event->wVirtualScanCode = (WORD)MapVirtualKeyW(virtual_key, MAPVK_VK_TO_VSC);
    key_event->uChar.UnicodeChar = symbol;
    key_event->dwControlKeyState = control_state;
}

// Ввести строку и Enter: пары "нажата/отпущена" для каждого символа.
static BOOL type_line(HANDLE input, const WCHAR *text)
{
    size_t symbol_count = wcslen(text) + 1;     // + Enter
    BUFFER records_buffer = { 0 };
    DWORD written = 0;

    if (buffer_init_array(&records_buffer, symbol_count * KEY_EVENTS_PER_SYMBOL, sizeof(INPUT_RECORD)) != BUFFER_OK)
    {
        return FALSE;
    }

    INPUT_RECORD *records = records_buffer.data;
    size_t record_count = 0;

    for (size_t i = 0; i < symbol_count; i++)
    {
        WCHAR symbol = (i < symbol_count - 1) ? text[i] : L'\r';
        DWORD control_state;
        WORD virtual_key = virtual_key_for_symbol(symbol, &control_state);

        fill_key_event(&records[record_count++], TRUE, symbol, virtual_key, control_state);
        fill_key_event(&records[record_count++], FALSE, symbol, virtual_key, control_state);
    }

    BOOL ok = WriteConsoleInputW(input, records, (DWORD)record_count, &written) && written == record_count;

    buffer_destroy(&records_buffer);

    return ok;
}

static BOOL console_measure(HANDLE output, HWND window, CONSOLE_METRICS *metrics,
                            CONSOLE_SCREEN_BUFFER_INFO *buffer_info)
{
    RECT window_rect, client_rect;

    if (!GetConsoleScreenBufferInfo(output, buffer_info) || !window) return FALSE;
    if (!GetWindowRect(window, &window_rect) || !GetClientRect(window, &client_rect)) return FALSE;

    metrics->columns = buffer_info->srWindow.Right - buffer_info->srWindow.Left + 1;
    metrics->rows = buffer_info->srWindow.Bottom - buffer_info->srWindow.Top + 1;
    metrics->font_width = metrics->columns > 0 ? client_rect.right / metrics->columns : 0;
    metrics->font_height = metrics->rows > 0 ? client_rect.bottom / metrics->rows : 0;

    // полосы прокрутки видны, если буфер больше окна
    int vertical_scrollbar = buffer_info->dwSize.Y > metrics->rows ? GetSystemMetrics(SM_CXVSCROLL) : 0;
    int horizontal_scrollbar = buffer_info->dwSize.X > metrics->columns ? GetSystemMetrics(SM_CYHSCROLL) : 0;

    metrics->frame_width = (window_rect.right - window_rect.left) - client_rect.right - vertical_scrollbar;
    metrics->frame_height = (window_rect.bottom - window_rect.top) - client_rect.bottom - horizontal_scrollbar;

    return metrics->font_width >= FONT_MIN_WIDTH && metrics->font_height >= FONT_MIN_HEIGHT;
}

// Сколько столбцов и строк влезет в окно width_px x height_px (вертикальная полоса прокрутки есть).
static void console_fit(const CONSOLE_METRICS *metrics, int width_px, int height_px, int *columns, int *rows)
{
    *columns = (width_px - metrics->frame_width - GetSystemMetrics(SM_CXVSCROLL)) / metrics->font_width;
    *rows = (height_px - metrics->frame_height) / metrics->font_height;

    if (*columns < MIN_COLUMNS) *columns = MIN_COLUMNS;
    if (*rows < MIN_ROWS) *rows = MIN_ROWS;
}

// Подогнать окно консоли под width_px x height_px пикселей: ширина буфера = ширине окна (без
// горизонтальной прокрутки), буфер не меньше 300 строк (вертикальная прокрутка есть, как
// обычно). Ширина обычно уже задана при запуске (con_startup_size) - здесь только высота.
static void fit_window(HANDLE output, HWND window, int width_px, int height_px)
{
    CONSOLE_SCREEN_BUFFER_INFO buffer_info;
    CONSOLE_METRICS metrics = { 0 };
    int columns, rows;
    COORD buffer_size;
    SMALL_RECT visible_area;

    if (!console_measure(output, window, &metrics, &buffer_info))
    {
        helper_write_line(TAG_MESSAGE, L"размер окна: не удалось узнать размер символа (окно %dx%d символов) - "
                          L"не меняю", metrics.columns, metrics.rows);
        return;
    }

    int old_columns = metrics.columns;
    int old_rows = metrics.rows;

    console_fit(&metrics, width_px, height_px, &columns, &rows);

    if (columns < buffer_info.dwSize.X)
    {
        helper_write_line(TAG_MESSAGE, L"внимание: буфер сужается с %d до %d столбцов - на XP/7 уже выведенные "
                          L"длинные строки обрежутся", buffer_info.dwSize.X, columns);
    }

    COORD largest = GetLargestConsoleWindowSize(output);

    if (largest.X > 0 && columns > largest.X) columns = largest.X;
    if (largest.Y > 0 && rows > largest.Y) rows = largest.Y;

    int buffer_rows = buffer_info.dwSize.Y;

    if (buffer_rows < MIN_BUFFER_ROWS) buffer_rows = MIN_BUFFER_ROWS;
    if (buffer_rows <= rows) buffer_rows = rows + 1;

    // окно - не больше ни старого, ни нового буфера; затем буфер; затем окно
    visible_area.Left = 0;
    visible_area.Top = 0;
    visible_area.Right = (SHORT)((old_columns < columns ? old_columns : columns) - 1);
    visible_area.Bottom = (SHORT)((old_rows < rows ? old_rows : rows) - 1);
    SetConsoleWindowInfo(output, TRUE, &visible_area);

    buffer_size.X = (SHORT)columns;
    buffer_size.Y = (SHORT)buffer_rows;

    if (!SetConsoleScreenBufferSize(output, buffer_size))
    {
        helper_write_line(TAG_MESSAGE, L"размер буфера %dx%d: ошибка %lu", columns, buffer_rows, GetLastError());
    }

    visible_area.Right = (SHORT)(columns - 1);
    visible_area.Bottom = (SHORT)(rows - 1);

    if (!SetConsoleWindowInfo(output, TRUE, &visible_area))
    {
        helper_write_line(TAG_MESSAGE, L"размер окна %dx%d: ошибка %lu", columns, rows, GetLastError());
    }

    helper_write_line(TAG_MESSAGE, L"окно консоли: символ %dx%d пикс., было %dx%d, стало %dx%d символов (буфер %d "
                      L"строк), заказано %dx%d пикс.", metrics.font_width, metrics.font_height, old_columns, old_rows,
                      columns, rows, buffer_rows, width_px, height_px);
}

// Уменьшить высоту окна по содержимому: строки до приглашения и две пустые, не меньше MIN_ROWS.
static void fit_content(HANDLE output)
{
    CONSOLE_SCREEN_BUFFER_INFO buffer_info;

    if (!GetConsoleScreenBufferInfo(output, &buffer_info)) return;

    int rows = buffer_info.srWindow.Bottom - buffer_info.srWindow.Top + 1;
    int needed_rows = buffer_info.dwCursorPosition.Y + CONTENT_EXTRA_ROWS;

    if (needed_rows < MIN_ROWS) needed_rows = MIN_ROWS;

    // содержимое не помещается или прокручено - не трогаем
    if (buffer_info.srWindow.Top != 0 || needed_rows >= rows) return;

    SMALL_RECT visible_area = buffer_info.srWindow;

    visible_area.Bottom = (SHORT)(needed_rows - 1);

    if (SetConsoleWindowInfo(output, TRUE, &visible_area))
    {
        helper_write_line(TAG_MESSAGE, L"высота окна по содержимому: %d строк", needed_rows);
    }
}

// Текст консоли от начала буфера до строки курсора - в файл результата.
static void dump_console_text(HANDLE output)
{
    CONSOLE_SCREEN_BUFFER_INFO buffer_info;
    BUFFER line_buffer = { 0 };

    if (!GetConsoleScreenBufferInfo(output, &buffer_info)) return;

    int columns = buffer_info.dwSize.X;

    if (columns > CONSOLE_MAX_COLUMNS) columns = CONSOLE_MAX_COLUMNS;
    if (buffer_init_array(&line_buffer, (size_t)columns + 1, sizeof(WCHAR)) != BUFFER_OK) return;

    WCHAR *line = line_buffer.data;

    for (int y = 0; y <= buffer_info.dwCursorPosition.Y; y++)
    {
        COORD start;
        DWORD read_count = 0;

        start.X = 0;
        start.Y = (SHORT)y;

        if (!ReadConsoleOutputCharacterW(output, line, (DWORD)columns, start, &read_count)) break;

        // пробелы в конце строки консоли - заполнитель, не текст
        while (read_count > 0 && line[read_count - 1] == L' ') read_count--;

        line[read_count] = 0;
        helper_write_line(TAG_CONSOLE_LINE, L"%s", line);
    }

    buffer_destroy(&line_buffer);
}

BOOL con_is_helper(int argc, WCHAR **argv)
{
    return argc >= 2 && skip_prefix(argv[1], HELPER_SWITCH) != NULL;
}

// "<ширина>x<высота>"
static void parse_size(const WCHAR *text, HELPER_ARGS *args)
{
    WCHAR *end;

    args->width_px = (int)wcstol(text, &end, DECIMAL_BASE);

    if (*end == L'x' || *end == L'X') args->height_px = (int)wcstol(end + 1, NULL, DECIMAL_BASE);
}

static void parse_helper_args(int argc, WCHAR **argv, HELPER_ARGS *args)
{
    ZeroMemory(args, sizeof(*args));
    args->command_timeout_ms = DEFAULT_COMMAND_TIMEOUT_MS;
    args->console_process_id = wcstoul(skip_prefix(argv[1], HELPER_SWITCH), NULL, DECIMAL_BASE);

    for (int i = 2; i < argc; i++)
    {
        const WCHAR *argument = argv[i];
        const WCHAR *size = skip_prefix(argument, ARG_SIZE);
        const WCHAR *timeout = skip_prefix(argument, ARG_TIMEOUT);
        const WCHAR *output_path = skip_prefix(argument, ARG_OUTPUT);
        const WCHAR *command = skip_prefix(argument, ARG_COMMAND);

        if (size)
        {
            parse_size(size, args);
        }
        else if (_wcsicmp(argument, ARG_FIT) == 0)
        {
            args->fit_content = TRUE;
        }
        else if (timeout)
        {
            args->command_timeout_ms = wcstoul(timeout, NULL, DECIMAL_BASE);
        }
        else if (output_path)
        {
            args->output_path = output_path;
        }
        else if (command && args->command_count < MAX_COMMANDS)
        {
            args->commands[args->command_count++] = command;
        }
    }
}

// Файл результата (UTF-16 с BOM): его читает основной процесс после выхода помощника.
static void open_result_file(const WCHAR *path)
{
    static const WCHAR bom = UTF16_BOM;
    DWORD written;

    g_result_file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY,
                                NULL);

    if (g_result_file != INVALID_HANDLE_VALUE) WriteFile(g_result_file, &bom, sizeof(bom), &written, NULL);
}

// Ввод или вывод консоли ("CONIN$" / "CONOUT$").
static HANDLE open_console_handle(const WCHAR *name)
{
    return CreateFileW(name, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                       0, NULL);
}

// Дождаться приглашения, подогнать окно, ввести команды и записать текст консоли.
static int run_commands(HANDLE input, HANDLE output, const HELPER_ARGS *args)
{
    IDLE_WAIT prompt_wait;
    IDLE_WAIT command_wait;
    COORD cursor;
    int exit_code = HELPER_EXIT_OK;

    prompt_wait.min_ms = PROMPT_MIN_MS;
    prompt_wait.quiet_ms = PROMPT_QUIET_MS;
    prompt_wait.timeout_ms = PROMPT_TIMEOUT_MS;

    command_wait.min_ms = COMMAND_MIN_MS;
    command_wait.quiet_ms = COMMAND_QUIET_MS;
    command_wait.timeout_ms = args->command_timeout_ms;

    // cmd только что запущен - дождаться приглашения
    if (!wait_idle(input, output, args->console_process_id, &prompt_wait, &cursor))
    {
        helper_write_line(TAG_MESSAGE, L"консоль не успокоилась за %lu с - ввожу команды всё равно",
                          (DWORD)(PROMPT_TIMEOUT_MS / MS_PER_SECOND));
    }
    else if (cursor.X == 0)
    {
        helper_write_line(TAG_MESSAGE, L"приглашения не видно (курсор в начале строки) - ввожу команды всё равно");
    }

    if (args->width_px > 0 && args->height_px > 0)
    {
        fit_window(output, GetConsoleWindow(), args->width_px, args->height_px);
    }

    for (int i = 0; i < args->command_count; i++)
    {
        const WCHAR *command = args->commands[i];
        DWORD start_tick = GetTickCount();

        if (!type_line(input, command))
        {
            helper_write_line(TAG_MESSAGE, L"\"%s\": ввод не удался (WriteConsoleInput), ошибка %lu", command,
                              GetLastError());
            exit_code = HELPER_EXIT_INPUT;
            break;
        }

        if (!wait_idle(input, output, args->console_process_id, &command_wait, &cursor))
        {
            helper_write_line(TAG_MESSAGE, L"\"%s\": не завершилась за %lu мс", command, args->command_timeout_ms);
            exit_code = HELPER_EXIT_TIMEOUT;
            break;
        }

        helper_write_line(TAG_MESSAGE, L"\"%s\": выполнена за %lu мс", command, GetTickCount() - start_tick);
    }

    if (args->fit_content && exit_code == HELPER_EXIT_OK) fit_content(output);

    dump_console_text(output);

    return exit_code;
}

// Подключиться к консоли cmd, выполнить команды и отключиться.
static int attach_and_run(const HELPER_ARGS *args)
{
    FreeConsole();      // помощник запускается без консоли - на всякий случай

    if (!AttachConsole(args->console_process_id))
    {
        helper_write_line(TAG_MESSAGE, L"AttachConsole(%lu): ошибка %lu", args->console_process_id, GetLastError());
        return HELPER_EXIT_ATTACH;
    }

    HANDLE input = open_console_handle(L"CONIN$");
    HANDLE output = open_console_handle(L"CONOUT$");
    int exit_code;

    if (input != INVALID_HANDLE_VALUE && output != INVALID_HANDLE_VALUE)
    {
        exit_code = run_commands(input, output, args);
    }
    else
    {
        helper_write_line(TAG_MESSAGE, L"не открыть CONIN$/CONOUT$: ошибка %lu", GetLastError());
        exit_code = HELPER_EXIT_HANDLES;
    }

    if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
    if (output != INVALID_HANDLE_VALUE) CloseHandle(output);

    FreeConsole();

    return exit_code;
}

int con_helper_main(int argc, WCHAR **argv)
{
    HELPER_ARGS args;
    int exit_code;

    parse_helper_args(argc, argv, &args);

    if (args.output_path) open_result_file(args.output_path);

    if (args.console_process_id)
    {
        exit_code = attach_and_run(&args);
    }
    else
    {
        helper_write_line(TAG_MESSAGE, L"не указан процесс консоли");
        exit_code = HELPER_EXIT_ARGS;
    }

    if (g_result_file != INVALID_HANDLE_VALUE) CloseHandle(g_result_file);

    return exit_code;
}

// Основной процесс

BOOL con_startup_size(int width_px, int height_px, STARTUPINFOW *startup_info)
{
    HWND console_window = GetConsoleWindow();
    CONSOLE_SCREEN_BUFFER_INFO buffer_info;
    CONSOLE_METRICS metrics;
    int columns, rows;

    if (!console_window || IsIconic(console_window)) return FALSE;

    // CONOUT$, а не стандартный вывод: он может быть перенаправлен в файл
    HANDLE output = open_console_handle(L"CONOUT$");

    if (output == INVALID_HANDLE_VALUE) return FALSE;

    BOOL measured = console_measure(output, console_window, &metrics, &buffer_info);

    CloseHandle(output);

    if (!measured) return FALSE;

    console_fit(&metrics, width_px, height_px, &columns, &rows);

    // одного буфера мало: окно по умолчанию 80 столбцов, и консоль расширяет буфер до ширины
    // окна (проверено на Win7). Размер окна в STARTF_USESIZE - в пикселях, консоль делит его на
    // размер символа: задаём ровно столбцы x строки
    startup_info->dwFlags |= STARTF_USECOUNTCHARS | STARTF_USESIZE;
    int buffer_rows = buffer_info.dwSize.Y > MIN_BUFFER_ROWS ? buffer_info.dwSize.Y : MIN_BUFFER_ROWS;

    startup_info->dwXCountChars = (DWORD)columns;
    startup_info->dwYCountChars = (DWORD)buffer_rows;
    startup_info->dwXSize = (DWORD)(columns * metrics.font_width);
    startup_info->dwYSize = (DWORD)(rows * metrics.font_height);

    log_msg(L"Консоль: по нашей консоли символ %dx%d пикс. - новая: буфер %lux%lu, окно %dx%d (под %dx%d пикс.)",
            metrics.font_width, metrics.font_height, startup_info->dwXCountChars, startup_info->dwYCountChars, columns,
            rows, width_px, height_px);

    return TRUE;
}

// Строка файла результата - в журнал. text_header_written - заголовок текста окна уже записан.
static void log_result_line(const WCHAR *line, BOOL *text_header_written)
{
    if (line[0] == TAG_MESSAGE)
    {
        log_msg(L"Консоль: %s", line + 1);
        return;
    }

    if (line[0] != TAG_CONSOLE_LINE) return;

    if (!*text_header_written)
    {
        log_msg(L"Консоль: текст окна:");
        *text_header_written = TRUE;
    }

    log_raw(L"    | %s", line + 1);
}

// Разбить текст файла результата на строки (меняет text) и перенести их в журнал.
static void log_result_text(WCHAR *text)
{
    BOOL text_header_written = FALSE;
    WCHAR *line = text;

    if (*line == UTF16_BOM) line++;

    while (*line)
    {
        WCHAR *next_line = line;

        while (*next_line && *next_line != L'\r' && *next_line != L'\n') next_line++;

        // конец строки "\r\n" или "\n" - заменить нулём и перешагнуть
        if (*next_line)
        {
            *next_line++ = 0;

            if (*next_line == L'\n') next_line++;
        }

        log_result_line(line, &text_header_written);
        line = next_line;
    }
}

// Перенести файл результата помощника в журнал.
static void log_helper_output(const WCHAR *path)
{
    BUFFER content = { 0 };
    DWORD bytes_read = 0;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);

    if (file == INVALID_HANDLE_VALUE)
    {
        log_msg(L"Консоль: нет файла результата помощника");
        return;
    }

    DWORD size = GetFileSize(file, NULL);

    // + место под завершающий ноль
    if (size == INVALID_FILE_SIZE || size > HELPER_OUTPUT_MAX_BYTES ||
        buffer_init(&content, size + sizeof(WCHAR)) != BUFFER_OK)
    {
        CloseHandle(file);
        return;
    }

    ReadFile(file, content.data, size, &bytes_read, NULL);
    CloseHandle(file);

    WCHAR *text = content.data;

    text[bytes_read / sizeof(WCHAR)] = 0;
    log_result_text(text);
    buffer_destroy(&content);
}

static const WCHAR *helper_exit_text(DWORD exit_code)
{
    switch (exit_code)
    {
        case HELPER_EXIT_OK:      return L"успешно";
        case HELPER_EXIT_ARGS:    return L"ошибка аргументов";
        case HELPER_EXIT_ATTACH:  return L"не подключиться к консоли";
        case HELPER_EXIT_HANDLES: return L"нет доступа к вводу/выводу консоли";
        case HELPER_EXIT_TIMEOUT: return L"команда не завершилась вовремя";
        case HELPER_EXIT_INPUT:   return L"ввод не удался";
        default:                  return L"неизвестный код";
    }
}

// Командная строка помощника в command_line (буфер выделяется здесь, освобождает вызывающий).
static BOOL build_helper_command_line(const HELPER_ARGS *args, const WCHAR *self_path, BUFFER *command_line)
{
    size_t capacity = MAX_PATH + PATH_BUFFER_CCH + HELPER_ARGS_RESERVE_CCH;

    for (int i = 0; i < args->command_count; i++) capacity += wcslen(args->commands[i]) + COMMAND_ARG_OVERHEAD_CCH;

    if (buffer_init_array(command_line, capacity, sizeof(WCHAR)) != BUFFER_OK) return FALSE;

    WCHAR *text = command_line->data;
    int text_cch = (int)capacity;
    int length = format_text(text, text_cch,
                             L"\"%s\" " HELPER_SWITCH L"%lu " ARG_SIZE L"%dx%d " ARG_TIMEOUT L"%lu%s "
                             L"\"" ARG_OUTPUT L"%s\"", self_path, args->console_process_id, args->width_px,
                             args->height_px, args->command_timeout_ms, args->fit_content ? L" " ARG_FIT : L"",
                             args->output_path);

    for (int i = 0; i < args->command_count && length >= 0; i++)
    {
        int added = format_text(text + length, text_cch - length, L" \"" ARG_COMMAND L"%s\"", args->commands[i]);

        length = added < 0 ? -1 : length + added;
    }

    return length >= 0;
}

// Ждать завершения помощника до limit_ms от start_tick (или аварийного прерывания).
// FALSE - не дождались.
static BOOL wait_helper(HANDLE process, DWORD limit_ms, DWORD start_tick)
{
    for (;;)
    {
        if (WaitForSingleObject(process, HELPER_POLL_MS) == WAIT_OBJECT_0) return TRUE;
        if (wnd_aborting() || GetTickCount() - start_tick >= limit_ms) return FALSE;
    }
}

BOOL con_type_commands(DWORD process_id, int width_px, int height_px, BOOL fit_content,
                       const WCHAR *const *commands, int command_count, DWORD command_timeout_ms)
{
    WCHAR self_path[MAX_PATH];
    WCHAR temp_dir[MAX_PATH];
    WCHAR result_path[PATH_BUFFER_CCH];
    WCHAR error_text[TEXT_BUFFER_CCH];
    HELPER_ARGS args;
    BUFFER command_line = { 0 };
    STARTUPINFOW startup_info;
    PROCESS_INFORMATION process_info;
    DWORD exit_code = HELPER_EXIT_UNKNOWN;

    if (wnd_aborting() || command_count <= 0 || command_count > MAX_COMMANDS) return FALSE;

    if (!GetModuleFileNameW(NULL, self_path, MAX_PATH))
    {
        log_msg(L"Консоль: не узнать путь к screens.exe: %s", log_errtext(GetLastError(), error_text, TEXT_BUFFER_CCH));
        return FALSE;
    }

    if (!GetTempPathW(MAX_PATH, temp_dir)) lstrcpyW(temp_dir, L".\\");

    format_text(result_path, PATH_BUFFER_CCH, L"%sscreens_con_%lu.txt", temp_dir, process_id);

    ZeroMemory(&args, sizeof(args));
    args.console_process_id = process_id;
    args.width_px = width_px;
    args.height_px = height_px;
    args.fit_content = fit_content;
    args.command_timeout_ms = command_timeout_ms;
    args.output_path = result_path;
    args.command_count = command_count;

    for (int i = 0; i < command_count; i++) args.commands[i] = commands[i];

    if (!build_helper_command_line(&args, self_path, &command_line))
    {
        buffer_destroy(&command_line);
        return FALSE;
    }

    ZeroMemory(&startup_info, sizeof(startup_info));
    startup_info.cb = sizeof(startup_info);
    ZeroMemory(&process_info, sizeof(process_info));

    // DETACHED_PROCESS: у помощника не должно быть своей консоли, иначе AttachConsole не выйдет
    BOOL started = CreateProcessW(self_path, command_line.data, NULL, NULL, FALSE, DETACHED_PROCESS, NULL, NULL,
                                  &startup_info, &process_info);
    DWORD start_error = GetLastError();

    buffer_destroy(&command_line);

    if (!started)
    {
        log_msg(L"Консоль: не запустить помощника: %s", log_errtext(start_error, error_text, TEXT_BUFFER_CCH));
        return FALSE;
    }

    CloseHandle(process_info.hThread);
    log_msg(L"Консоль: ввод %d команд в cmd PID %lu (помощник PID %lu)", command_count, process_id,
            process_info.dwProcessId);

    DWORD limit_ms = command_timeout_ms * (DWORD)command_count + HELPER_START_RESERVE_MS;
    DWORD start_tick = GetTickCount();
    BOOL finished = wait_helper(process_info.hProcess, limit_ms, start_tick);

    if (!finished)
    {
        TerminateProcess(process_info.hProcess, TERMINATED_EXIT_CODE);
        WaitForSingleObject(process_info.hProcess, TERMINATE_WAIT_MS);
        log_msg(L"Консоль: помощник не закончил за %lu мс - завершён", GetTickCount() - start_tick);
    }

    GetExitCodeProcess(process_info.hProcess, &exit_code);
    CloseHandle(process_info.hProcess);
    log_helper_output(result_path);
    DeleteFileW(result_path);

    if (finished)
    {
        log_msg(L"Консоль: помощник закончил за %lu мс, код %lu (%s)", GetTickCount() - start_tick, exit_code,
                helper_exit_text(exit_code));
    }

    return finished && exit_code == HELPER_EXIT_OK;
}
