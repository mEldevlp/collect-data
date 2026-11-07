// Раздел logs - :collect_logs в collect_audit.cmd: список файлов *.log из %WINDIR% и %WINDIR%\Inf
// (08_файлы_log\08_список_файлов_log.txt, вывод dir - make_md.ps1 ищет в нём "не найден") и их копии
// в 08_файлы_log\WINDIR и WINDIR_Inf. Скрипт копировал xcopy, программа - CopyFileW: так же сохраняется время
// изменения, снимается "только чтение" (как у xcopy без /k), папка создаётся, только если есть что копировать.
#include "sections.h"
#include "common.h"
#include "log.h"

#define LOG_MASK L"*.log"

typedef struct
{
    const WCHAR *subdir;        // папка внутри %WINDIR% ("" - сама %WINDIR%)
    const WCHAR *target;        // папка копий внутри 08_файлы_log
} LOG_SOURCE;

static const LOG_SOURCE LOG_SOURCES[] =
{
    { L"",     L"WINDIR" },
    { L"\\Inf", L"WINDIR_Inf" },
};

// %WINDIR%, как у скрипта; нет переменной - GetSystemWindowsDirectoryW.
static void windows_dir(WCHAR *dir, int dir_cch)
{
    DWORD length = GetEnvironmentVariableW(L"WINDIR", dir, (DWORD)dir_cch);

    if (length > 0 && length < (DWORD)dir_cch) return;

    UINT system_length = GetSystemWindowsDirectoryW(dir, (UINT)dir_cch);

    if (system_length == 0 || system_length >= (UINT)dir_cch) lstrcpynW(dir, L"C:\\Windows", dir_cch);
}

static BOOL list_logs(COLLECT_CONTEXT *context, OUT_FILE *list, const WCHAR *source_dir)
{
    WCHAR title[PATH_BUFFER_CCH];
    WCHAR arguments[PATH_BUFFER_CCH];

    format_text(title, PATH_BUFFER_CCH, L"dir %s\\%s", source_dir, LOG_MASK);
    format_text(arguments, PATH_BUFFER_CCH, L"/c dir /a-d \"%s\\%s\"", source_dir, LOG_MASK);

    return collect_command(context, list, SECT_LOGS_LIST_FILE, title, L"cmd.exe", arguments);
}

// Одна копия: время изменения CopyFileW сохраняет; "только чтение" снимаем, как xcopy без /k.
static BOOL copy_log(const WCHAR *source, const WCHAR *target)
{
    if (!CopyFileW(source, target, FALSE)) return FALSE;

    DWORD attributes = GetFileAttributesW(target);

    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY))
    {
        SetFileAttributesW(target, attributes & ~FILE_ATTRIBUTE_READONLY);
    }

    return TRUE;
}

// Копии *.log из source_dir в 08_файлы_log\<target_name>. Не скопированные (заняты системой) - в журнал, раздел
// при этом не считается неудачным: xcopy /c у скрипта тоже шёл дальше. FALSE - не создать папку копий.
static BOOL copy_logs(COLLECT_CONTEXT *context, const WCHAR *source_dir, const WCHAR *target_name)
{
    WCHAR relative_dir[PATH_BUFFER_CCH];
    WCHAR target_dir[PATH_BUFFER_CCH];
    WCHAR pattern[PATH_BUFFER_CCH];
    WCHAR command[PATH_BUFFER_CCH];
    WCHAR error_text[TEXT_BUFFER_CCH];
    WIN32_FIND_DATAW found;
    int copied = 0;
    int failed = 0;

    format_text(relative_dir, PATH_BUFFER_CCH, L"%s\\%s", SECT_LOGS_DIR, target_name);
    collect_full_path(context, relative_dir, target_dir, PATH_BUFFER_CCH);
    format_text(pattern, PATH_BUFFER_CCH, L"%s\\%s", source_dir, LOG_MASK);
    format_text(command, PATH_BUFFER_CCH, L"xcopy \"%s\" \"%s\\\" /c /h /y (WinAPI: FindFirstFileW, CopyFileW)",
                pattern, relative_dir);
    collect_note_command(context, command, relative_dir);

    HANDLE search = FindFirstFileW(pattern, &found);

    if (search == INVALID_HANDLE_VALUE)
    {
        log_msg(L"Файлов %s в %s нет", LOG_MASK, source_dir);
        return TRUE;
    }

    BOOL dir_ready = FALSE;
    BOOL dir_failed = FALSE;

    do
    {
        WCHAR source[PATH_BUFFER_CCH];
        WCHAR target[PATH_BUFFER_CCH];

        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

        // папка копий - только когда есть первый файл (у скрипта её тоже не было, если копировать нечего)
        if (!dir_ready)
        {
            dir_ready = collect_create_dir(context, relative_dir);
            dir_failed = !dir_ready;

            if (dir_failed) break;
        }

        format_text(source, PATH_BUFFER_CCH, L"%s\\%s", source_dir, found.cFileName);
        format_text(target, PATH_BUFFER_CCH, L"%s\\%s", target_dir, found.cFileName);

        if (copy_log(source, target))
        {
            copied++;
            continue;
        }

        failed++;
        log_msg(L"Не скопирован %s: %s", source, log_errtext(GetLastError(), error_text, TEXT_BUFFER_CCH));
    }
    while (FindNextFileW(search, &found));

    FindClose(search);
    log_msg(L"Скопировано файлов %s из %s: %d, не скопировано: %d", LOG_MASK, source_dir, copied, failed);

    return !dir_failed;
}

BOOL sect_logs(COLLECT_CONTEXT *context)
{
    WCHAR windows[MAX_PATH];
    OUT_FILE list;
    BOOL ok = TRUE;

    windows_dir(windows, MAX_PATH);

    if (!collect_open_output(context, SECT_LOGS_LIST_FILE, &list)) return FALSE;

    for (int i = 0; i < (int)ARRAY_COUNT(LOG_SOURCES); i++)
    {
        WCHAR source_dir[PATH_BUFFER_CCH];

        format_text(source_dir, PATH_BUFFER_CCH, L"%s%s", windows, LOG_SOURCES[i].subdir);
        ok = list_logs(context, &list, source_dir) && ok;
    }

    if (list.failed)
    {
        log_msg(L"Файл %s записан не полностью", SECT_LOGS_LIST_FILE);
        ok = FALSE;
    }

    out_close(&list);

    for (int i = 0; i < (int)ARRAY_COUNT(LOG_SOURCES); i++)
    {
        WCHAR source_dir[PATH_BUFFER_CCH];

        format_text(source_dir, PATH_BUFFER_CCH, L"%s%s", windows, LOG_SOURCES[i].subdir);
        ok = copy_logs(context, source_dir, LOG_SOURCES[i].target) && ok;
    }

    return ok;
}
