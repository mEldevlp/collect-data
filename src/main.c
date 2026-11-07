// screens.exe - скриншоты для collect_audit.cmd.
// screens.exe <папка_результатов> [/only:имя1,имя2] [/list] [/keep] [/dump] [/timeout:сек] [/primary]
// Коды возврата: 0 - все снимки сделаны, 1 - часть не удалась, 2 - снимать нельзя / ошибка запуска.

#include <windows.h>
#include <limits.h>
#include <stdlib.h>
#include "common.h"
#include "console.h"
#include "log.h"
#include "osinfo.h"
#include "scenarios.h"
#include "wnd.h"

#define SCREENS_VERSION L"0.1"

#define EXIT_OK      0
#define EXIT_PARTIAL 1
#define EXIT_CANNOT  2

// parse_arguments: аргументы разобраны, работаем дальше (иначе - код выхода)
#define PARSE_CONTINUE (-1)

#define DEFAULT_TIMEOUT_SEC 300     // общий лимит работы
#define DEFAULT_WAIT_SEC    45      // сколько ждать заполнения окна (список программ)
#define DEFAULT_MAX_PAGES   10      // страниц длинного списка ПО
#define MAX_PAGES_LIMIT     100     // /pages: больше не принимаем

#define WATCHDOG_STOP_WAIT_MS 2000  // ожидание выхода сторожевого потока после конца работы

#define DRIVE_ROOT_LENGTH 3         // "C:\" - у корня диска завершающий \ не убираем

// Параметры командной строки
typedef struct
{
    const WCHAR *results_dir;
    const WCHAR *only;              // список сценариев из /only или NULL
    BOOL         list;
    BOOL         keep;
    BOOL         dump;
    BOOL         primary_only;      // снимать только основной монитор
    long         timeout_sec;
    long         wait_sec;
    long         max_pages;
} OPTIONS;

static HANDLE g_done_event;
static DWORD  g_timeout_sec = DEFAULT_TIMEOUT_SEC;

static void usage(void)
{
    log_raw(L"screens.exe %s - снимки экрана для collect_audit.cmd", SCREENS_VERSION);
    log_raw(L"");
    log_raw(L"  screens.exe <папка> [/only:имя1,имя2] [/list] [/keep] [/dump] [/timeout:сек] [/wait:сек] [/pages:N] "
            L"[/primary]");
    log_raw(L"");
    log_raw(L"  /only:...    снять только указанные сценарии (имена - см. /list)");
    log_raw(L"  /list        список сценариев");
    log_raw(L"  /keep        не закрывать открытые окна (отладка)");
    log_raw(L"  /dump        записать в журнал дерево окон и MMC");
    log_raw(L"  /timeout:N   общий лимит работы в секундах (по умолчанию %d)", DEFAULT_TIMEOUT_SEC);
    log_raw(L"  /wait:N      сколько ждать заполнения окна, например списка программ (по умолчанию %d)",
            DEFAULT_WAIT_SEC);
    log_raw(L"  /pages:N     длинный список ПО - не больше N страниц (по умолчанию %d)", DEFAULT_MAX_PAGES);
    log_raw(L"  /primary     снимать только основной монитор (по умолчанию - все мониторы)");
    log_raw(L"");
    log_raw(L"  Коды возврата: 0 - всё снято, 1 - часть не удалась, 2 - снимать нельзя");
}

static void list_scenarios(void)
{
    log_raw(L"Сценарии:");

    for (int i = 0; i < g_scenario_count; i++)
    {
        const SCENARIO *scenario = &g_scenarios[i];

        log_raw(L"  %-18s %-26s %s%s%s", scenario->id, scenario->file_name, scenario->title,
                scenario->hidden ? L" [только по /only]" : L"", scenario->run ? L"" : L" [не реализован]");
    }
}

// Входит ли id в список "a,b,c" (без учёта регистра).
static BOOL in_list(const WCHAR *list, const WCHAR *id)
{
    size_t id_length = wcslen(id);
    const WCHAR *cursor = list;
    const WCHAR *item;
    size_t item_length;

    while (next_list_item(&cursor, &item, &item_length))
    {
        if (item_length == id_length && _wcsnicmp(item, id, id_length) == 0) return TRUE;
    }

    return FALSE;
}

// Есть ли сценарий с именем name (длиной length, без учёта регистра).
static BOOL scenario_exists(const WCHAR *name, size_t length)
{
    for (int i = 0; i < g_scenario_count; i++)
    {
        const WCHAR *id = g_scenarios[i].id;

        if (wcslen(id) == length && _wcsnicmp(name, id, length) == 0) return TRUE;
    }

    return FALSE;
}

// Проверить, что все имена из /only существуют; неизвестные - предупреждение в журнал.
static void check_only_list(const WCHAR *only)
{
    const WCHAR *cursor = only;
    const WCHAR *item;
    size_t item_length;

    while (next_list_item(&cursor, &item, &item_length))
    {
        if (item_length > 0 && !scenario_exists(item, item_length))
        {
            log_msg(L"Внимание: неизвестный сценарий в /only: %.*s", (int)item_length, item);
        }
    }
}

static DWORD WINAPI watchdog_proc(LPVOID arg)
{
    (void)arg;

    if (WaitForSingleObject(g_done_event, g_timeout_sec * MS_PER_SECOND) == WAIT_TIMEOUT)
    {
        log_msg(L"СТОРОЖЕВОЙ ТАЙМЕР: работа идёт дольше %lu с, аварийное завершение", g_timeout_sec);
        wnd_emergency_cleanup();
        log_close();
        ExitProcess(EXIT_PARTIAL);
    }

    return 0;
}

static BOOL WINAPI console_ctrl_handler(DWORD control_type)
{
    (void)control_type;

    log_msg(L"Прервано (Ctrl+C / закрытие консоли)");
    wnd_emergency_cleanup();
    log_close();

    return FALSE;   // дальше - стандартная обработка (завершение)
}

// Создать папку, если её нет (только последний уровень).
static BOOL ensure_dir(const WCHAR *dir)
{
    DWORD attributes = GetFileAttributesW(dir);

    if (attributes != INVALID_FILE_ATTRIBUTES) return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

    return CreateDirectoryW(dir, NULL);
}

// Полный путь папки результатов без завершающего \ (кроме корня диска "C:\").
static void full_results_dir(const WCHAR *results_dir, WCHAR *full_path, int full_path_cch)
{
    DWORD length = GetFullPathNameW(results_dir, (DWORD)full_path_cch, full_path, NULL);

    if (length == 0 || length >= (DWORD)full_path_cch) lstrcpynW(full_path, results_dir, full_path_cch);

    size_t path_length = wcslen(full_path);
    WCHAR last = path_length > 0 ? full_path[path_length - 1] : 0;

    if (path_length > DRIVE_ROOT_LENGTH && (last == L'\\' || last == L'/')) full_path[path_length - 1] = 0;
}

// Число из ключа "/имя:N": больше 0 и не больше max_value - в *value, иначе значение не меняется.
static void parse_positive(const WCHAR *text, long max_value, long *value)
{
    long number = wcstol(text, NULL, DECIMAL_BASE);

    if (number > 0 && number <= max_value) *value = number;
}

// Ключ: "/..." или "-..." (но не "-" и не путь вида "-\...").
static BOOL is_switch(const WCHAR *argument)
{
    return argument[0] == L'/' || (argument[0] == L'-' && argument[1] != 0 && argument[1] != L'\\');
}

// Разобрать один ключ. PARSE_CONTINUE - дальше, иначе код выхода (справка или ошибка).
static int parse_switch(const WCHAR *argument, OPTIONS *options)
{
    const WCHAR *name = argument + 1;
    const WCHAR *only = skip_prefix(name, L"only:");
    const WCHAR *timeout = skip_prefix(name, L"timeout:");
    const WCHAR *pages = skip_prefix(name, L"pages:");
    const WCHAR *wait = skip_prefix(name, L"wait:");

    if (_wcsicmp(name, L"list") == 0)
    {
        options->list = TRUE;
    }
    else if (_wcsicmp(name, L"keep") == 0)
    {
        options->keep = TRUE;
    }
    else if (_wcsicmp(name, L"dump") == 0)
    {
        options->dump = TRUE;
    }
    else if (_wcsicmp(name, L"primary") == 0)
    {
        options->primary_only = TRUE;
    }
    else if (_wcsicmp(name, L"allmonitors") == 0)
    {
        options->primary_only = FALSE;
    }
    else if (only)
    {
        options->only = only;
    }
    else if (timeout)
    {
        parse_positive(timeout, LONG_MAX, &options->timeout_sec);
    }
    else if (pages)
    {
        parse_positive(pages, MAX_PAGES_LIMIT, &options->max_pages);
    }
    else if (wait)
    {
        parse_positive(wait, LONG_MAX, &options->wait_sec);
    }
    else if (_wcsicmp(name, L"?") == 0 || _wcsicmp(name, L"help") == 0)
    {
        usage();
        return EXIT_OK;
    }
    else
    {
        log_raw(L"Неизвестный ключ: %s", argument);
        usage();
        return EXIT_CANNOT;
    }

    return PARSE_CONTINUE;
}

// Разобрать командную строку. PARSE_CONTINUE - работать дальше, иначе код выхода.
static int parse_arguments(int argc, WCHAR **argv, OPTIONS *options)
{
    ZeroMemory(options, sizeof(*options));
    options->timeout_sec = DEFAULT_TIMEOUT_SEC;
    options->wait_sec = DEFAULT_WAIT_SEC;
    options->max_pages = DEFAULT_MAX_PAGES;

    for (int i = 1; i < argc; i++)
    {
        const WCHAR *argument = argv[i];

        if (is_switch(argument))
        {
            int result = parse_switch(argument, options);

            if (result != PARSE_CONTINUE) return result;

            continue;
        }

        if (options->results_dir)
        {
            log_raw(L"Лишний аргумент: %s", argument);
            usage();
            return EXIT_CANNOT;
        }

        options->results_dir = argument;
    }

    return PARSE_CONTINUE;
}

// Выполнить выбранные сценарии. *total - сколько выбрано; возвращает, сколько удалось.
static int run_scenarios(const SCEN_CONTEXT *context, const WCHAR *only, int *total)
{
    int succeeded = 0;

    *total = 0;

    for (int i = 0; i < g_scenario_count; i++)
    {
        const SCENARIO *scenario = &g_scenarios[i];
        BOOL selected = only ? in_list(only, scenario->id) : !scenario->hidden;

        if (!selected) continue;

        (*total)++;
        log_msg(L"--- Сценарий %s: %s", scenario->id, scenario->title);

        if (!scenario->run)
        {
            log_msg(L"Сценарий %s пока не реализован, пропущен", scenario->id);
            continue;
        }

        DWORD start_tick = GetTickCount();
        BOOL ok = scenario->run(context, scenario);

        log_msg(L"Сценарий %s: %s за %lu мс", scenario->id, ok ? L"успешно" : L"НЕ УДАЛСЯ",
                GetTickCount() - start_tick);

        if (ok) succeeded++;
    }

    return succeeded;
}

int wmain(int argc, WCHAR **argv)
{
    OPTIONS options;
    WCHAR results_dir[PATH_BUFFER_CCH];
    WCHAR reason[MESSAGE_BUFFER_CCH];
    WCHAR error_text[TEXT_BUFFER_CCH];
    OSINFO os_info;
    SCEN_CONTEXT context;
    int total = 0;

    // до любых GetSystemMetrics: иначе на Win10 с масштабом > 100% размеры экрана занижены
    os_set_dpi_aware();

    // служебный запуск: помощник для окна cmd (см. console.h) - без журнала и проверок
    if (con_is_helper(argc, argv)) return con_helper_main(argc, argv);

    int parse_result = parse_arguments(argc, argv, &options);

    if (parse_result != PARSE_CONTINUE) return parse_result;

    if (options.list)
    {
        list_scenarios();
        return EXIT_OK;
    }

    if (!options.results_dir)
    {
        usage();
        return EXIT_CANNOT;
    }

    full_results_dir(options.results_dir, results_dir, PATH_BUFFER_CCH);

    if (!ensure_dir(results_dir))
    {
        log_raw(L"Не удалось создать папку %s: %s", results_dir,
                log_errtext(GetLastError(), error_text, TEXT_BUFFER_CCH));
        return EXIT_CANNOT;
    }

    if (!log_open(results_dir))
    {
        log_raw(L"Внимание: не удалось открыть журнал в %s: %s", results_dir,
                log_errtext(GetLastError(), error_text, TEXT_BUFFER_CCH));
    }

    log_raw(L"");
    log_msg(L"=== screens.exe %s, запуск: %s", SCREENS_VERSION, GetCommandLineW());
    os_get(&os_info);
    os_log(&os_info);

    if (!os_can_capture(reason, MESSAGE_BUFFER_CCH))
    {
        log_msg(L"Снимать нельзя: %s", reason);
        log_msg(L"=== Завершено, код %d", EXIT_CANNOT);
        log_close();
        return EXIT_CANNOT;
    }

    g_timeout_sec = (DWORD)options.timeout_sec;
    g_done_event = CreateEventW(NULL, TRUE, FALSE, NULL);

    HANDLE watchdog = CreateThread(NULL, 0, watchdog_proc, NULL, 0, NULL);

    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);

    ZeroMemory(&context, sizeof(context));
    context.results_dir = results_dir;
    context.all_monitors = !options.primary_only;
    context.keep = options.keep;
    context.dump = options.dump;
    context.wait_ms = (DWORD)options.wait_sec * MS_PER_SECOND;
    context.max_pages = (int)options.max_pages;
    context.os_info = &os_info;

    if (options.only) check_only_list(options.only);

    int succeeded = run_scenarios(&context, options.only, &total);

    // окно сведений о системе и всё, что осталось открытым
    wnd_close_all(options.keep);

    SetEvent(g_done_event);

    if (watchdog)
    {
        WaitForSingleObject(watchdog, WATCHDOG_STOP_WAIT_MS);
        CloseHandle(watchdog);
    }

    int exit_code = EXIT_CANNOT;

    if (total == 0) log_msg(L"Не выбрано ни одного сценария");
    else exit_code = (succeeded == total) ? EXIT_OK : EXIT_PARTIAL;

    log_msg(L"=== Завершено: снято %d из %d, код %d", succeeded, total, exit_code);
    log_close();

    return exit_code;
}
