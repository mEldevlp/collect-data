// Раздел sysinfo - :collect_sysinfo в collect_audit.cmd: 00_общие_сведения\00_сведения_о_системе.txt.
// Подписи в начале файла - английские, как у скрипта: по ним make_md.ps1 строит шапку отчёта.
#include "sections.h"
#include "common.h"
#include "log.h"
#include <lmcons.h>     // UNLEN, DNLEN - наибольшие длины имени пользователя и домена

// Значения GetNativeSystemInfo, которых нет в SDK 7.1A
#define ARCHITECTURE_ARM64 12   // PROCESSOR_ARCHITECTURE_ARM64

#define ARCHITECTURE_CCH 16     // "AMD64", "x86" или номер

// Имя архитектуры - как %PROCESSOR_ARCHITECTURE%; NULL - неизвестная.
static const WCHAR *architecture_name(WORD architecture)
{
    switch (architecture)
    {
        case PROCESSOR_ARCHITECTURE_AMD64: return L"AMD64";
        case PROCESSOR_ARCHITECTURE_INTEL: return L"x86";
        case PROCESSOR_ARCHITECTURE_IA64:  return L"IA64";
        case ARCHITECTURE_ARM64:           return L"ARM64";
        default:                           return NULL;
    }
}

// Архитектура ОС, а не нашего процесса (32-битная сборка в 64-битной Windows видит "x86") - как
// %PROCESSOR_ARCHITECTURE% у 64-битного cmd, в котором перезапускал себя скрипт.
static void native_architecture(WCHAR *text, int text_cch)
{
    SYSTEM_INFO info;

    GetNativeSystemInfo(&info);

    const WCHAR *name = architecture_name(info.wProcessorArchitecture);

    if (name) lstrcpynW(text, name, text_cch);
    else format_text(text, text_cch, L"%u", info.wProcessorArchitecture);
}

// Переменная окружения; нет - fallback.
static void environment_value(const WCHAR *name, const WCHAR *fallback, WCHAR *value, int value_cch)
{
    DWORD length = GetEnvironmentVariableW(name, value, (DWORD)value_cch);

    if (length == 0 || length >= (DWORD)value_cch) lstrcpynW(value, fallback, value_cch);
}

// "Run as": %USERDOMAIN%\%USERNAME%, как у скрипта; переменных нет - GetUserNameW и имя компьютера.
static void run_as(const COLLECT_CONTEXT *context, WCHAR *domain, int domain_cch, WCHAR *user, int user_cch)
{
    WCHAR own_name[UNLEN + 1];
    DWORD own_name_cch = ARRAY_COUNT(own_name);

    if (!GetUserNameW(own_name, &own_name_cch)) lstrcpyW(own_name, L"?");

    environment_value(L"USERDOMAIN", context->computer_name, domain, domain_cch);
    environment_value(L"USERNAME", own_name, user, user_cch);
}

static void write_header_lines(COLLECT_CONTEXT *context, OUT_FILE *file)
{
    WCHAR domain[DNLEN + 1];
    WCHAR user[UNLEN + 1];
    WCHAR architecture[ARCHITECTURE_CCH];
    SYSTEMTIME now;

    GetLocalTime(&now);
    run_as(context, domain, ARRAY_COUNT(domain), user, ARRAY_COUNT(user));
    native_architecture(architecture, ARCHITECTURE_CCH);

    out_line(file, L"Collection date: %02u.%02u.%04u %02u:%02u:%02u", now.wDay, now.wMonth, now.wYear, now.wHour,
             now.wMinute, now.wSecond);
    out_line(file, L"Computer name: %s", context->computer_name);
    out_line(file, L"IP address: %s", context->ip_address);
    out_line(file, L"Run as: %s\\%s", domain, user);
    out_line(file, L"Administrator rights: %s", context->options->os_info->is_admin ? L"yes" : L"no");
    out_line(file, L"Processor architecture: %s", architecture);

    collect_note_command(context, L"(WinAPI) дата, имя компьютера, IP, пользователь, права, архитектура: "
                         L"GetLocalTime, %COMPUTERNAME%, GetIpForwardTable, %USERDOMAIN%\\%USERNAME%, "
                         L"CheckTokenMembership, GetNativeSystemInfo", SECT_SYSINFO_FILE);
}

BOOL sect_sysinfo(COLLECT_CONTEXT *context)
{
    OUT_FILE file;

    if (!collect_open_output(context, SECT_SYSINFO_FILE, &file)) return FALSE;

    write_header_lines(context, &file);

    BOOL ok = collect_command(context, &file, SECT_SYSINFO_FILE, L"ver", L"cmd.exe", L"/c ver");

    // [Win32_OperatingSystem] и [Win32_ComputerSystem] (WMI) - этап 7, часть 2
    ok = collect_command(context, &file, SECT_SYSINFO_FILE, L"systeminfo", L"systeminfo.exe", NULL) && ok;
    ok = collect_command(context, &file, SECT_SYSINFO_FILE, L"ipconfig /all", L"ipconfig.exe", L"/all") && ok;

    if (file.failed)
    {
        log_msg(L"Файл %s записан не полностью", SECT_SYSINFO_FILE);
        ok = FALSE;
    }

    out_close(&file);

    return ok;
}
