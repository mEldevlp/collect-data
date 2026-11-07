#include "collect.h"
#include "common.h"
#include "exec.h"
#include "log.h"
#include "sections.h"
#include "text.h"

#define COMMANDS_FILE L"_команды_сбора.txt"

// Сколько ждать каждую программу сбора (на медленной XP systeminfo идёт десятки секунд)
#define COMMAND_TIMEOUT_SEC 120

// Вывод длиннее предела exec.h: в файл результата - пометка, в мегабайтах
#define BYTES_PER_MEGABYTE     (1024 * 1024)
#define OUTPUT_LIMIT_MEGABYTES (EXEC_OUTPUT_MAX_BYTES / BYTES_PER_MEGABYTE)

#define NETWORK_SECTION_ID L"network"

#define NO_OUTPUT_TEXT  L"вывод не сохраняется"
#define COMMAND_ARROW   L"    -> "

static const WCHAR *const SYSINFO_OUTPUTS[]  = { SECT_SYSINFO_FILE, NULL };
static const WCHAR *const SOFTWARE_OUTPUTS[] = { SECT_SOFTWARE_DIR, NULL };
static const WCHAR *const TIME_OUTPUTS[]     = { SECT_TIME_DIR, NULL };
static const WCHAR *const AUDIT_OUTPUTS[]    = { SECT_AUDIT_DIR, NULL };
static const WCHAR *const ACCOUNTS_OUTPUTS[] = { SECT_ACCOUNTS_DIR, NULL };
static const WCHAR *const USERS_OUTPUTS[]    = { SECT_USERS_DIR, NULL };
static const WCHAR *const SERVICES_OUTPUTS[] = { SECT_SERVICES_DIR, NULL };
static const WCHAR *const REMOTE_OUTPUTS[]   = { SECT_REMOTE_DIR, NULL };
static const WCHAR *const LOGS_OUTPUTS[]     = { SECT_LOGS_DIR, NULL };
static const WCHAR *const NETWORK_OUTPUTS[]  = { L"*" SECT_NETWORK_SUFFIX, NULL };

// Порядок - как у collect_audit.cmd
const COLLECT_SECTION g_collect_sections[] =
{
    { L"sysinfo",  L"Сведения о системе",                                   SYSINFO_OUTPUTS,  sect_sysinfo },
    { L"software", L"Установленное ПО, обновления и компоненты Windows",    SOFTWARE_OUTPUTS, NULL },
    { L"time",     L"Синхронизация времени",                                TIME_OUTPUTS,     NULL },
    { L"audit",    L"Политика аудита и параметры журналов событий",         AUDIT_OUTPUTS,    NULL },
    { L"accounts", L"Политики учётных записей",                             ACCOUNTS_OUTPUTS, NULL },
    { L"users",    L"Учётные записи и группы",                              USERS_OUTPUTS,    NULL },
    { L"services", L"Системные службы",                                     SERVICES_OUTPUTS, NULL },
    { L"remote",   L"Удалённое администрирование",                          REMOTE_OUTPUTS,   NULL },
    { L"logs",     L"Файлы *.log из %WINDIR% и %WINDIR%\\Inf",              LOGS_OUTPUTS,     sect_logs },
    { L"network",  L"Сетевые связи",                                        NETWORK_OUTPUTS,  NULL },
};

const int g_collect_section_count = ARRAY_COUNT(g_collect_sections);

void collect_list(void)
{
    log_raw(L"Разделы сбора данных:");

    for (int i = 0; i < g_collect_section_count; i++)
    {
        const COLLECT_SECTION *section = &g_collect_sections[i];

        log_raw(L"  %-18s %s%s", section->id, section->title, section->run ? L"" : L" [ещё не перенесён]");
    }
}

BOOL collect_section_exists(const WCHAR *name, size_t length)
{
    for (int i = 0; i < g_collect_section_count; i++)
    {
        const WCHAR *id = g_collect_sections[i].id;

        if (wcslen(id) == length && _wcsnicmp(name, id, length) == 0) return TRUE;
    }

    return FALSE;
}

static BOOL section_selected(const COLLECT_OPTIONS *options, const COLLECT_SECTION *section)
{
    if (options->no_network && lstrcmpiW(section->id, NETWORK_SECTION_ID) == 0) return FALSE;

    return options->only ? list_contains(options->only, section->id) : TRUE;
}

BOOL collect_any_selected(const COLLECT_OPTIONS *options)
{
    for (int i = 0; i < g_collect_section_count; i++)
    {
        if (section_selected(options, &g_collect_sections[i])) return TRUE;
    }

    return FALSE;
}

void collect_full_path(const COLLECT_CONTEXT *context, const WCHAR *relative_path, WCHAR *path, int path_cch)
{
    format_text(path, path_cch, L"%s\\%s", context->options->results_dir, relative_path);
}

static BOOL is_dot_entry(const WCHAR *name)
{
    return lstrcmpW(name, L".") == 0 || lstrcmpW(name, L"..") == 0;
}

static BOOL delete_tree(const WCHAR *path);

static void delete_children(const WCHAR *dir)
{
    WCHAR pattern[PATH_BUFFER_CCH];
    WIN32_FIND_DATAW found;

    format_text(pattern, PATH_BUFFER_CCH, L"%s\\*", dir);

    HANDLE search = FindFirstFileW(pattern, &found);

    if (search == INVALID_HANDLE_VALUE) return;

    do
    {
        WCHAR child[PATH_BUFFER_CCH];

        if (is_dot_entry(found.cFileName)) continue;

        format_text(child, PATH_BUFFER_CCH, L"%s\\%s", dir, found.cFileName);
        delete_tree(child);
    }
    while (FindNextFileW(search, &found));

    FindClose(search);
}

// Удалить файл или папку со всем содержимым (атрибут "только чтение" снимается: так его ставят копии *.log).
// Ссылки на другие папки (точки повторной обработки) не раскрываются - удаляется сама ссылка.
// TRUE - удалено или этого пути и не было.
static BOOL delete_tree(const WCHAR *path)
{
    DWORD attributes = GetFileAttributesW(path);

    if (attributes == INVALID_FILE_ATTRIBUTES)
    {
        DWORD error = GetLastError();

        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
    }

    if (attributes & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(path, attributes & ~FILE_ATTRIBUTE_READONLY);

    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) return DeleteFileW(path);

    if (!(attributes & FILE_ATTRIBUTE_REPARSE_POINT)) delete_children(path);

    return RemoveDirectoryW(path);
}

// Удалить прежний результат: путь внутри папки результатов (есть - удаляется, в журнал).
static void delete_previous(const COLLECT_CONTEXT *context, const WCHAR *relative_path)
{
    WCHAR path[PATH_BUFFER_CCH];
    WCHAR error_text[TEXT_BUFFER_CCH];

    collect_full_path(context, relative_path, path, PATH_BUFFER_CCH);

    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return;

    if (delete_tree(path))
    {
        log_msg(L"Удалён прежний результат: %s", relative_path);
        return;
    }

    log_msg(L"Внимание: не удалось удалить прежний результат %s: %s", relative_path,
            log_errtext(GetLastError(), error_text, TEXT_BUFFER_CCH));
}

// Прежние файлы по маске в корне папки результатов ("*_сетевые_связи.txt").
static void delete_previous_matching(const COLLECT_CONTEXT *context, const WCHAR *mask)
{
    WCHAR pattern[PATH_BUFFER_CCH];
    WIN32_FIND_DATAW found;

    collect_full_path(context, mask, pattern, PATH_BUFFER_CCH);

    HANDLE search = FindFirstFileW(pattern, &found);

    if (search == INVALID_HANDLE_VALUE) return;

    do
    {
        if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) delete_previous(context, found.cFileName);
    }
    while (FindNextFileW(search, &found));

    FindClose(search);
}

// Перед сбором - прежние результаты тех разделов, которые сейчас будут собраны (снимки PNG и журналы
// не трогаются, результаты невыбранных и ещё не перенесённых разделов - тоже).
static void remove_previous_outputs(const COLLECT_CONTEXT *context)
{
    for (int i = 0; i < g_collect_section_count; i++)
    {
        const COLLECT_SECTION *section = &g_collect_sections[i];

        if (!section->run || !section_selected(context->options, section)) continue;

        for (const WCHAR *const *output = section->outputs; *output; output++)
        {
            if ((*output)[0] == L'*') delete_previous_matching(context, *output);
            else delete_previous(context, *output);
        }
    }

    delete_previous(context, COMMANDS_FILE);
}

BOOL collect_create_dir(const COLLECT_CONTEXT *context, const WCHAR *relative_dir)
{
    WCHAR path[PATH_BUFFER_CCH];
    WCHAR error_text[TEXT_BUFFER_CCH];
    size_t root_length = wcslen(context->options->results_dir) + 1;

    collect_full_path(context, relative_dir, path, PATH_BUFFER_CCH);

    // все уровни после папки результатов: "00_общие_сведения", затем "00_общие_сведения\..."
    for (WCHAR *cursor = path + root_length; ; cursor++)
    {
        WCHAR symbol = *cursor;

        if (symbol != L'\\' && symbol != 0) continue;

        *cursor = 0;

        if (!CreateDirectoryW(path, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        {
            log_msg(L"Не удалось создать папку %s: %s", path, log_errtext(GetLastError(), error_text, TEXT_BUFFER_CCH));
            return FALSE;
        }

        *cursor = symbol;

        if (symbol == 0) return TRUE;
    }
}

BOOL collect_open_output(const COLLECT_CONTEXT *context, const WCHAR *relative_path, OUT_FILE *file)
{
    WCHAR dir[PATH_BUFFER_CCH];
    WCHAR path[PATH_BUFFER_CCH];
    WCHAR error_text[TEXT_BUFFER_CCH];

    lstrcpynW(dir, relative_path, PATH_BUFFER_CCH);

    WCHAR *last_slash = wcsrchr(dir, L'\\');

    if (last_slash)
    {
        *last_slash = 0;

        if (!collect_create_dir(context, dir))
        {
            file->handle = INVALID_HANDLE_VALUE;
            file->failed = TRUE;
            return FALSE;
        }
    }

    collect_full_path(context, relative_path, path, PATH_BUFFER_CCH);

    if (out_open(file, path)) return TRUE;

    log_msg(L"Не удалось открыть файл результата %s: %s", relative_path,
            log_errtext(GetLastError(), error_text, TEXT_BUFFER_CCH));

    return FALSE;
}

void collect_note_command(COLLECT_CONTEXT *context, const WCHAR *command, const WCHAR *relative_target)
{
    out_line(&context->commands, L"%s", command);
    out_line(&context->commands, L"%s%s", COMMAND_ARROW, relative_target ? relative_target : NO_OUTPUT_TEXT);
}

// Итог команды - в файл результата (если вывода нет или он неполный) и в журнал.
static BOOL report_command(OUT_FILE *file, const WCHAR *title, const EXEC_RESULT *result)
{
    WCHAR error_text[TEXT_BUFFER_CCH];

    if (result->not_found)
    {
        out_line(file, L"программы нет в этой системе: %s", result->command_line);
        log_msg(L"%s: программы нет в этой системе (%s)", title, result->command_line);
        return TRUE;
    }

    if (!result->started)
    {
        log_errtext(result->error, error_text, TEXT_BUFFER_CCH);
        out_line(file, L"не удалось запустить %s: %s", result->command_line, error_text);
        log_msg(L"%s: НЕ ЗАПУСТИЛАСЬ (%s): %s", title, result->command_line, error_text);
        return FALSE;
    }

    if (result->truncated)
    {
        out_line(file, L"вывод длиннее %d МБ, записано только начало", OUTPUT_LIMIT_MEGABYTES);
    }

    if (result->timed_out)
    {
        out_line(file, L"команда не завершилась за %d с и остановлена", COMMAND_TIMEOUT_SEC);
        log_msg(L"%s: НЕ ЗАВЕРШИЛАСЬ за %d с, остановлена", title, COMMAND_TIMEOUT_SEC);
        return FALSE;
    }

    if (result->error)
    {
        log_msg(L"%s: не удалось прочитать вывод: %s", title, log_errtext(result->error, error_text, TEXT_BUFFER_CCH));
        return FALSE;
    }

    log_msg(L"%s: код %lu, %lu мс", title, result->exit_code, result->elapsed_ms);

    return TRUE;
}

BOOL collect_command(COLLECT_CONTEXT *context, OUT_FILE *file, const WCHAR *relative_path, const WCHAR *title,
                     const WCHAR *program, const WCHAR *arguments)
{
    EXEC_REQUEST request;
    EXEC_RESULT result;

    ZeroMemory(&request, sizeof(request));
    request.program = program;
    request.arguments = arguments;
    request.timeout_ms = COMMAND_TIMEOUT_SEC * MS_PER_SECOND;

    out_header(file, title);
    exec_run(&request, &result);
    collect_note_command(context, result.command_line, relative_path);

    if (result.text.data) out_text(file, result.text.data, text_length(&result.text));

    BOOL ok = report_command(file, title, &result);

    exec_free(&result);

    return ok;
}

static void init_context(COLLECT_CONTEXT *context, const COLLECT_OPTIONS *options)
{
    ZeroMemory(context, sizeof(*context));
    context->options = options;
    context->commands.handle = INVALID_HANDLE_VALUE;
    os_computer_name(context->computer_name, OS_COMPUTER_NAME_CCH);

    if (!os_primary_ipv4(context->ip_address, OS_IPV4_TEXT_CCH)) lstrcpyW(context->ip_address, OS_NO_IP_TEXT);
}

// Файл команд: заново при каждом сборе, шапка - как у collect_audit.cmd. Не открылся - причина уже в журнале,
// записи в него пропадают.
static void open_commands(COLLECT_CONTEXT *context)
{
    SYSTEMTIME now;

    if (!collect_open_output(context, COMMANDS_FILE, &context->commands)) return;

    GetLocalTime(&now);
    out_line(&context->commands, L"Команды, которые выполнила программа screens.exe %s, в порядке выполнения.",
             SCREENS_VERSION);
    out_line(&context->commands, L"Компьютер: %s, IP %s, %02u.%02u.%04u %02u:%02u:%02u", context->computer_name,
             context->ip_address, now.wDay, now.wMonth, now.wYear, now.wHour, now.wMinute, now.wSecond);
    out_line(&context->commands, L"");
    out_line(&context->commands, L"Строки с решёткой - разделы сбора, как в журнале сбора. Под каждой командой после "
             L"стрелки - файл, в который записан её вывод.");
    out_line(&context->commands, L"Пути внутри папки результатов указаны относительно неё.");
    out_line(&context->commands, L"Действия, которые программа выполняет сама, функциями WinAPI, без запуска других "
             L"программ, записаны равнозначной командой с пометкой (WinAPI).");
}

static void log_start(const COLLECT_CONTEXT *context)
{
    const COLLECT_OPTIONS *options = context->options;
    const OSINFO *os_info = options->os_info;

    log_msg(L"=== screens.exe %s, сбор данных, запуск: %s", SCREENS_VERSION, GetCommandLineW());
    log_msg(L"Папка результатов: %s", options->results_dir);

    if (options->folder_note && options->folder_note[0]) log_msg(L"Внимание: %s", options->folder_note);

    log_msg(L"Начало сбора: компьютер %s, IP %s, ОС %lu.%lu.%lu %s, права администратора: %s",
            context->computer_name, context->ip_address, os_info->major, os_info->minor, os_info->build,
            os_info->service_pack, os_info->is_admin ? L"да" : L"нет");

    if (!os_info->is_admin) log_msg(L"Внимание: нет прав администратора - часть сведений не будет собрана");
}

int collect_run(const COLLECT_OPTIONS *options)
{
    COLLECT_CONTEXT context;
    int done = 0;
    int failed = 0;
    int not_ported = 0;
    DWORD start_tick = GetTickCount();

    init_context(&context, options);
    log_start(&context);
    remove_previous_outputs(&context);
    open_commands(&context);

    for (int i = 0; i < g_collect_section_count; i++)
    {
        const COLLECT_SECTION *section = &g_collect_sections[i];

        if (!section_selected(options, section)) continue;

        log_msg(L"--- Раздел %s: %s", section->id, section->title);

        if (!section->run)
        {
            log_msg(L"Раздел %s ещё не перенесён из collect_audit.cmd, пропущен", section->id);
            not_ported++;
            continue;
        }

        out_line(&context.commands, L"");
        out_line(&context.commands, L"# %s", section->title);

        DWORD section_tick = GetTickCount();
        BOOL ok = section->run(&context);

        log_msg(L"Раздел %s: %s за %lu мс", section->id, ok ? L"успешно" : L"НЕ УДАЛСЯ", GetTickCount() - section_tick);

        if (ok) done++;
        else failed++;
    }

    if (context.commands.failed) log_msg(L"Внимание: файл команд %s записан не полностью", COMMANDS_FILE);

    out_close(&context.commands);
    log_msg(L"=== Сбор завершён за %lu с: разделов выполнено %d, не удалось %d, ещё не перенесено %d",
            (GetTickCount() - start_tick) / MS_PER_SECOND, done, failed, not_ported);
    log_msg(L"Finished");

    return failed;
}
