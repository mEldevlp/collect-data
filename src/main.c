// screens.exe - сбор данных и скриншоты для аудита ИБ.
// screens.exe [папка_результатов] [/only:имя1,имя2] [/list] [/nocollect] [/noscreens] [/nonet] [/ping] [/nopause]
//             [/keep] [/dump] [/timeout:сек] [/wait:сек] [/pages:N] [/primary]
// Без папки - <папка exe>\<ИМЯ_ПК>_<IP> (нельзя писать - %SystemDrive%\<ИМЯ_ПК>_<IP>), уже есть - туда же.
// Сначала сбор данных (collect.c), затем снимки (scenarios.c).
// Коды возврата: 0 - всё собрано и снято, 1 - часть не удалась, 2 - снимать нельзя / ошибка запуска.

#include <windows.h>
#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "collect.h"
#include "common.h"
#include "console.h"
#include "exec.h"
#include "log.h"
#include "osinfo.h"
#include "scenarios.h"
#include "wnd.h"

#define EXIT_OK      0
#define EXIT_PARTIAL 1
#define EXIT_CANNOT  2

// parse_arguments: аргументы разобраны, работаем дальше (иначе - код выхода)
#define PARSE_CONTINUE (-1)

#define DEFAULT_TIMEOUT_SEC 1200    // общий лимит работы: сбор данных и снимки
#define DEFAULT_WAIT_SEC    45      // сколько ждать заполнения окна (список программ)
#define DEFAULT_MAX_PAGES   10      // страниц длинного списка ПО
#define MAX_PAGES_LIMIT     100     // /pages: больше не принимаем

#define WATCHDOG_STOP_WAIT_MS 2000  // ожидание выхода сторожевого потока после конца работы

#define DRIVE_ROOT_LENGTH 3         // "C:\" - у корня диска завершающий \ не убираем

// Папка результатов по умолчанию: <ИМЯ_ПК>_<IP>, как у collect_audit.cmd
#define FOLDER_NAME_CCH  (OS_COMPUTER_NAME_CCH + 1 + OS_IPV4_TEXT_CCH)
#define WRITE_TEST_FILE  L"_проверка_записи.tmp"    // создаётся и сразу удаляется: можно ли писать в папку

// Пауза в конце: чтобы отличить "к консоли подключены только мы" от "мы и cmd", хватает двух мест
#define CONSOLE_PROCESS_PROBE 2

// Параметры командной строки
typedef struct
{
    const WCHAR *results_dir;
    const WCHAR *only;              // список сценариев и разделов сбора из /only или NULL
    BOOL         list;
    BOOL         no_collect;        // /nocollect - только снимки
    BOOL         no_screens;        // /noscreens - только сбор данных
    BOOL         no_network;        // /nonet - без сетевых связей
    BOOL         ping;              // /ping - опрос подключённых подсетей (сетевые связи)
    BOOL         no_pause;          // /nopause - не ждать нажатия клавиши
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
    log_raw(L"screens.exe %s - сбор данных и снимки экрана для аудита ИБ", SCREENS_VERSION);
    log_raw(L"");
    log_raw(L"  screens.exe [папка] [/only:имя1,имя2] [/list] [/nocollect] [/noscreens] [/nonet] [/ping] [/nopause]");
    log_raw(L"              [/keep] [/dump] [/timeout:сек] [/wait:сек] [/pages:N] [/primary]");
    log_raw(L"");
    log_raw(L"  папка        куда класть результаты; без неё - <ИМЯ_ПК>_<IP> рядом с exe (если там писать нельзя -");
    log_raw(L"               на системном диске); папка уже есть - результаты кладутся в неё");
    log_raw(L"  /only:...    только указанные разделы сбора и сценарии снимков (имена - см. /list)");
    log_raw(L"  /list        список разделов сбора и сценариев");
    log_raw(L"  /nocollect   без сбора данных, только снимки");
    log_raw(L"  /noscreens   без снимков, только сбор данных");
    log_raw(L"  /nonet       без сетевых связей");
    log_raw(L"  /ping        сетевые связи: опросить ping подключённые подсети (системы обнаружения вторжений могут");
    log_raw(L"               принять это за сканирование сети)");
    log_raw(L"  /nopause     не ждать нажатия клавиши (в конце и при запуске без прав администратора)");
    log_raw(L"  /keep        не закрывать открытые окна (отладка)");
    log_raw(L"  /dump        записать в журнал дерево окон и MMC");
    log_raw(L"  /timeout:N   общий лимит работы в секундах (по умолчанию %d)", DEFAULT_TIMEOUT_SEC);
    log_raw(L"  /wait:N      сколько ждать заполнения окна, например списка программ (по умолчанию %d)",
            DEFAULT_WAIT_SEC);
    log_raw(L"  /pages:N     длинный список ПО - не больше N страниц (по умолчанию %d)", DEFAULT_MAX_PAGES);
    log_raw(L"  /primary     снимать только основной монитор (по умолчанию - все мониторы)");
    log_raw(L"");
    log_raw(L"  Коды возврата: 0 - всё собрано и снято, 1 - часть не удалась, 2 - снимать нельзя");
}

static void list_scenarios(void)
{
    log_raw(L"Сценарии снимков:");

    for (int i = 0; i < g_scenario_count; i++)
    {
        const SCENARIO *scenario = &g_scenarios[i];

        log_raw(L"  %-18s %-26s %s%s%s", scenario->id, scenario->file_name, scenario->title,
                scenario->hidden ? L" [только по /only]" : L"", scenario->run ? L"" : L" [не реализован]");
    }
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

static BOOL scenario_selected(const SCENARIO *scenario, const WCHAR *only)
{
    return only ? list_contains(only, scenario->id) : !scenario->hidden;
}

static BOOL any_scenario_selected(const WCHAR *only)
{
    for (int i = 0; i < g_scenario_count; i++)
    {
        if (scenario_selected(&g_scenarios[i], only)) return TRUE;
    }

    return FALSE;
}

// Проверить, что все имена из /only существуют (среди сценариев и разделов сбора); неизвестные - предупреждение.
static void check_only_list(const WCHAR *only)
{
    const WCHAR *cursor = only;
    const WCHAR *item;
    size_t item_length;

    while (next_list_item(&cursor, &item, &item_length))
    {
        if (item_length > 0 && !scenario_exists(item, item_length) && !collect_section_exists(item, item_length))
        {
            log_msg(L"Внимание: неизвестное имя в /only: %.*s", (int)item_length, item);
        }
    }
}

static DWORD WINAPI watchdog_proc(LPVOID arg)
{
    (void)arg;

    if (WaitForSingleObject(g_done_event, g_timeout_sec * MS_PER_SECOND) == WAIT_TIMEOUT)
    {
        log_msg(L"СТОРОЖЕВОЙ ТАЙМЕР: работа идёт дольше %lu с, аварийное завершение", g_timeout_sec);
        exec_abort();
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
    exec_abort();
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

// Имя папки результатов по умолчанию: <ИМЯ_ПК>_<IP> - как %COMPUTERNAME%_%IP% у collect_audit.cmd.
static void default_folder_name(WCHAR *name, int name_cch)
{
    WCHAR computer[OS_COMPUTER_NAME_CCH];
    WCHAR address[OS_IPV4_TEXT_CCH];

    os_computer_name(computer, OS_COMPUTER_NAME_CCH);

    if (!os_primary_ipv4(address, OS_IPV4_TEXT_CCH)) lstrcpyW(address, OS_NO_IP_TEXT);

    format_text(name, name_cch, L"%s_%s", computer, address);
}

// Папка, где лежит сам exe (а не текущая: при "Запуск от имени администратора" текущая - system32).
// Для exe в корне диска - "C:" (к ней добавляется "\имя"). FALSE - не узнать.
static BOOL exe_dir(WCHAR *dir, int dir_cch)
{
    DWORD length = GetModuleFileNameW(NULL, dir, (DWORD)dir_cch);

    if (length == 0 || length >= (DWORD)dir_cch) return FALSE;

    WCHAR *last_slash = wcsrchr(dir, L'\\');

    if (!last_slash) return FALSE;

    *last_slash = 0;

    return TRUE;
}

// В папке можно создать файл (папка может остаться от прошлого раза на защищённом от записи носителе).
static BOOL dir_writable(const WCHAR *dir)
{
    WCHAR path[PATH_BUFFER_CCH];

    format_text(path, PATH_BUFFER_CCH, L"%s\\%s", dir, WRITE_TEST_FILE);

    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);

    if (file == INVALID_HANDLE_VALUE) return FALSE;

    CloseHandle(file);

    return TRUE;
}

// Папка есть (создана) и в неё можно писать. FALSE - GetLastError объясняет почему.
static BOOL prepare_dir(const WCHAR *dir)
{
    return ensure_dir(dir) && dir_writable(dir);
}

// Папка результатов, когда она не указана: <папка exe>\<ИМЯ_ПК>_<IP>; там писать нельзя -
// %SystemDrive%\<ИМЯ_ПК>_<IP> (в note - что взята запасная папка и почему). Папка уже есть - она же.
// FALSE - не годится ни одна (причина на экране).
static BOOL choose_default_dir(WCHAR *results_dir, int results_dir_cch, WCHAR *note, int note_cch)
{
    WCHAR name[FOLDER_NAME_CCH];
    WCHAR base[PATH_BUFFER_CCH];
    WCHAR system_drive[MAX_PATH];
    WCHAR error_text[TEXT_BUFFER_CCH];

    note[0] = 0;
    default_folder_name(name, FOLDER_NAME_CCH);

    if (exe_dir(base, PATH_BUFFER_CCH))
    {
        format_text(results_dir, results_dir_cch, L"%s\\%s", base, name);

        if (prepare_dir(results_dir)) return TRUE;

        format_text(note, note_cch, L"рядом с exe писать нельзя (%s: %s), взята папка на системном диске",
                    results_dir, log_errtext(GetLastError(), error_text, TEXT_BUFFER_CCH));
    }

    DWORD length = GetEnvironmentVariableW(L"SystemDrive", system_drive, MAX_PATH);

    if (length == 0 || length >= MAX_PATH) lstrcpyW(system_drive, L"C:");

    format_text(results_dir, results_dir_cch, L"%s\\%s", system_drive, name);

    if (prepare_dir(results_dir)) return TRUE;

    log_raw(L"Не удалось создать папку %s или записать в неё: %s", results_dir,
            log_errtext(GetLastError(), error_text, TEXT_BUFFER_CCH));

    return FALSE;
}

// Консоль создана для нас (запуск из Проводника, "Запуск от имени администратора"): кроме нас к ней
// никто не подключён, и окно закроется сразу по выходе. Из cmd и других программ - FALSE.
static BOOL own_console(void)
{
    DWORD process_ids[CONSOLE_PROCESS_PROBE];

    return GetConsoleProcessList(process_ids, CONSOLE_PROCESS_PROBE) == 1;
}

// Ждать нажатия клавиши (нажатия, сделанные раньше, не в счёт).
static void read_key(void)
{
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    INPUT_RECORD record;
    DWORD read_count = 0;

    FlushConsoleInputBuffer(input);

    while (ReadConsoleInputW(input, &record, 1, &read_count) && read_count > 0)
    {
        if (record.EventType == KEY_EVENT && record.Event.KeyEvent.bKeyDown) break;
    }
}

// Пауза в конце (только на экран: журнал к этому времени закрыт).
static void wait_key(void)
{
    log_raw(L"");
    log_raw(L"Нажмите любую клавишу, чтобы закрыть окно...");
    read_key();
}

// Без прав администратора часть сведений не собрать: предупреждение, а при запуске из Проводника - пауза,
// чтобы можно было закрыть окно и запустить программу "от имени администратора" (как у collect_audit.cmd).
static void warn_not_admin(BOOL allow_pause)
{
    log_raw(L"");
    log_raw(L"ВНИМАНИЕ: программа запущена без прав администратора.");
    log_raw(L"Политика безопасности, политика аудита и часть других сведений не будут собраны.");

    if (!allow_pause) return;

    log_raw(L"Закройте окно и запустите программу \"от имени администратора\" или нажмите любую клавишу,");
    log_raw(L"чтобы продолжить без этих прав.");
    read_key();
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

// Ключ без значения: имя, поле OPTIONS (смещение - инициализатор таблицы должен быть константой) и значение
typedef struct
{
    const WCHAR *name;
    size_t       offset;
    BOOL         value;
} FLAG_SWITCH;

static const FLAG_SWITCH FLAG_SWITCHES[] =
{
    { L"list",        offsetof(OPTIONS, list),         TRUE },
    { L"nocollect",   offsetof(OPTIONS, no_collect),   TRUE },
    { L"noscreens",   offsetof(OPTIONS, no_screens),   TRUE },
    { L"nonet",       offsetof(OPTIONS, no_network),   TRUE },
    { L"ping",        offsetof(OPTIONS, ping),         TRUE },
    { L"nopause",     offsetof(OPTIONS, no_pause),     TRUE },
    { L"keep",        offsetof(OPTIONS, keep),         TRUE },
    { L"dump",        offsetof(OPTIONS, dump),         TRUE },
    { L"primary",     offsetof(OPTIONS, primary_only), TRUE },
    { L"allmonitors", offsetof(OPTIONS, primary_only), FALSE },
};

// Ключ без значения (FLAG_SWITCHES). TRUE - такой ключ есть.
static BOOL parse_flag(const WCHAR *name, OPTIONS *options)
{
    for (int i = 0; i < (int)ARRAY_COUNT(FLAG_SWITCHES); i++)
    {
        const FLAG_SWITCH *flag = &FLAG_SWITCHES[i];

        if (_wcsicmp(name, flag->name) != 0) continue;

        *(BOOL *)((BYTE *)options + flag->offset) = flag->value;
        return TRUE;
    }

    return FALSE;
}

// Разобрать один ключ. PARSE_CONTINUE - дальше, иначе код выхода (справка или ошибка).
static int parse_switch(const WCHAR *argument, OPTIONS *options)
{
    const WCHAR *name = argument + 1;
    const WCHAR *only = skip_prefix(name, L"only:");
    const WCHAR *timeout = skip_prefix(name, L"timeout:");
    const WCHAR *pages = skip_prefix(name, L"pages:");
    const WCHAR *wait = skip_prefix(name, L"wait:");

    if (parse_flag(name, options))
    {
        return PARSE_CONTINUE;
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

        if (!scenario_selected(scenario, only)) continue;

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

// Папка результатов: указанная в командной строке (создаётся, если её нет) или по умолчанию
// (choose_default_dir). note - что записать в журнал о выборе папки. FALSE - папки нет (причина на экране).
static BOOL prepare_results_dir(const OPTIONS *options, WCHAR *results_dir, WCHAR *note, int note_cch)
{
    WCHAR error_text[TEXT_BUFFER_CCH];

    note[0] = 0;

    if (!options->results_dir) return choose_default_dir(results_dir, PATH_BUFFER_CCH, note, note_cch);

    full_results_dir(options->results_dir, results_dir, PATH_BUFFER_CCH);

    if (ensure_dir(results_dir)) return TRUE;

    log_raw(L"Не удалось создать папку %s: %s", results_dir, log_errtext(GetLastError(), error_text, TEXT_BUFFER_CCH));

    return FALSE;
}

// Открыть журнал в папке результатов; не открылся - пишем только в консоль.
static void open_log(const WCHAR *results_dir, const WCHAR *file_name)
{
    WCHAR error_text[TEXT_BUFFER_CCH];

    if (log_open(results_dir, file_name)) return;

    log_raw(L"Внимание: не удалось открыть журнал %s в %s: %s", file_name, results_dir,
            log_errtext(GetLastError(), error_text, TEXT_BUFFER_CCH));
}

// Сбор данных со своим журналом. Возвращает число разделов, которые не удались.
static int run_collect(const COLLECT_OPTIONS *collect_options)
{
    open_log(collect_options->results_dir, LOG_COLLECT_FILE);

    int failed = collect_run(collect_options);

    log_close();

    return failed;
}

// Снимки со своим журналом. Возвращает код выхода снимков.
static int run_screens(const OPTIONS *options, const WCHAR *results_dir, const WCHAR *note, const OSINFO *os_info)
{
    WCHAR reason[MESSAGE_BUFFER_CCH];
    SCEN_CONTEXT context;
    int total = 0;

    open_log(results_dir, LOG_SCREENS_FILE);
    log_raw(L"");
    log_msg(L"=== screens.exe %s, запуск: %s", SCREENS_VERSION, GetCommandLineW());
    log_msg(L"Папка результатов: %s", results_dir);

    if (note[0]) log_msg(L"Внимание: %s", note);

    os_log(os_info);

    if (!os_can_capture(reason, MESSAGE_BUFFER_CCH))
    {
        log_msg(L"Снимать нельзя: %s", reason);
        log_msg(L"=== Завершено, код %d", EXIT_CANNOT);
        log_close();
        return EXIT_CANNOT;
    }

    ZeroMemory(&context, sizeof(context));
    context.results_dir = results_dir;
    context.all_monitors = !options->primary_only;
    context.keep = options->keep;
    context.dump = options->dump;
    context.wait_ms = (DWORD)options->wait_sec * MS_PER_SECOND;
    context.max_pages = (int)options->max_pages;
    context.os_info = os_info;

    int succeeded = run_scenarios(&context, options->only, &total);

    // окно сведений о системе и всё, что осталось открытым
    wnd_close_all(options->keep);

    int exit_code = EXIT_CANNOT;

    if (total == 0) log_msg(L"Не выбрано ни одного сценария");
    else exit_code = (succeeded == total) ? EXIT_OK : EXIT_PARTIAL;

    log_msg(L"=== Завершено: снято %d из %d, код %d", succeeded, total, exit_code);
    log_close();

    return exit_code;
}

// Сторожевой таймер на всю работу: сбор и снимки. NULL - поток не создан (работаем без сторожа).
static HANDLE start_watchdog(long timeout_sec)
{
    g_timeout_sec = (DWORD)timeout_sec;
    g_done_event = CreateEventW(NULL, TRUE, FALSE, NULL);

    return CreateThread(NULL, 0, watchdog_proc, NULL, 0, NULL);
}

static void stop_watchdog(HANDLE watchdog)
{
    SetEvent(g_done_event);

    if (!watchdog) return;

    WaitForSingleObject(watchdog, WATCHDOG_STOP_WAIT_MS);
    CloseHandle(watchdog);
}

// Вся работа утилиты (кроме служебного запуска помощника). Возвращает код выхода.
// interactive - окно консоли наше (запуск из Проводника); *allow_pause - можно ли ждать клавишу в конце.
static int run(int argc, WCHAR **argv, BOOL interactive, BOOL *allow_pause)
{
    OPTIONS options;
    COLLECT_OPTIONS collect_options;
    WCHAR results_dir[PATH_BUFFER_CCH];
    WCHAR note[MESSAGE_BUFFER_CCH];
    OSINFO os_info;

    int parse_result = parse_arguments(argc, argv, &options);

    *allow_pause = !options.no_pause;

    if (parse_result != PARSE_CONTINUE) return parse_result;

    if (options.list)
    {
        collect_list();
        log_raw(L"");
        list_scenarios();
        return EXIT_OK;
    }

    if (!prepare_results_dir(&options, results_dir, note, MESSAGE_BUFFER_CCH)) return EXIT_CANNOT;

    os_get(&os_info);

    ZeroMemory(&collect_options, sizeof(collect_options));
    collect_options.results_dir = results_dir;
    collect_options.os_info = &os_info;
    collect_options.folder_note = note;
    collect_options.only = options.only;
    collect_options.no_network = options.no_network;
    collect_options.ping = options.ping;

    BOOL want_collect = !options.no_collect && collect_any_selected(&collect_options);
    BOOL want_screens = !options.no_screens && any_scenario_selected(options.only);

    // до журналов: предупреждение - только на экран (и в _stdout.txt прогона на ВМ)
    if (options.only) check_only_list(options.only);

    if (!want_collect && !want_screens)
    {
        log_raw(L"Не выбрано ни одного раздела сбора или сценария снимков");
        return EXIT_CANNOT;
    }

    if (!os_info.is_admin) warn_not_admin(interactive && !options.no_pause);

    HANDLE watchdog = start_watchdog(options.timeout_sec);

    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);

    int collect_failed = want_collect ? run_collect(&collect_options) : 0;
    int exit_code = want_screens ? run_screens(&options, results_dir, note, &os_info) : EXIT_OK;

    stop_watchdog(watchdog);

    if (exit_code == EXIT_OK && collect_failed > 0) exit_code = EXIT_PARTIAL;

    return exit_code;
}

int wmain(int argc, WCHAR **argv)
{
    // до любых GetSystemMetrics: иначе на Win10 с масштабом > 100% размеры экрана занижены
    os_set_dpi_aware();

    // служебный запуск: помощник для окна cmd (см. console.h) - без журнала и проверок
    if (con_is_helper(argc, argv)) return con_helper_main(argc, argv);

    // запуск из Проводника: окно консоли наше и закроется сразу - итог должен успеть прочитать человек
    BOOL interactive = own_console();
    BOOL allow_pause = TRUE;
    int exit_code = run(argc, argv, interactive, &allow_pause);

    if (interactive && allow_pause) wait_key();

    return exit_code;
}
