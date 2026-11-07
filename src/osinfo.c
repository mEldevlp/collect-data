#include "osinfo.h"
#include "common.h"
#include "log.h"
#include <lmcons.h>     // UNLEN - наибольшая длина имени пользователя

// Значения DPI_AWARENESS_CONTEXT из SDK Windows 10 (в SDK 7.1A для XP их ещё нет)
#define DPI_CONTEXT_PER_MONITOR_AWARE_V2 ((HANDLE)(INT_PTR)-4)     // Windows 10 1703+
#define DPI_CONTEXT_PER_MONITOR_AWARE    ((HANDLE)(INT_PTR)-3)

// PROCESS_PER_MONITOR_DPI_AWARE из shellscalingapi.h (Windows 8.1+)
#define PROCESS_PER_MONITOR_DPI_AWARE_VALUE 2

// SID группы Администраторы S-1-5-32-544: две подчинённые части - BUILTIN (32) и Администраторы (544)
#define ADMINS_SID_SUBAUTHORITY_COUNT 2

// Имя станции окон или рабочего стола: "WinSta0", "Default", "Winlogon"...
#define OBJECT_NAME_CCH 128

#define INTERACTIVE_WINDOW_STATION L"WinSta0"
#define DEFAULT_DESKTOP            L"Default"

typedef LONG (WINAPI *PFN_RtlGetVersion)(OSVERSIONINFOEXW *);
typedef BOOL (WINAPI *PFN_IsWow64Process)(HANDLE, PBOOL);
typedef LANGID (WINAPI *PFN_GetUserDefaultUILanguage)(void);
typedef BOOL (WINAPI *PFN_SetProcessDPIAware)(void);
typedef HRESULT (WINAPI *PFN_SetProcessDpiAwareness)(int);
typedef BOOL (WINAPI *PFN_SetProcessDpiAwarenessContext)(HANDLE);

// Версия ОС: RtlGetVersion не врёт про версию (GetVersionEx на 8.1+ без манифеста говорит 6.2).
static void read_version(OSVERSIONINFOEXW *version)
{
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    PFN_RtlGetVersion rtl_get_version = NULL;

    ZeroMemory(version, sizeof(*version));
    version->dwOSVersionInfoSize = sizeof(*version);

    if (ntdll) rtl_get_version = (PFN_RtlGetVersion)GetProcAddress(ntdll, "RtlGetVersion");

    if (rtl_get_version && rtl_get_version(version) == 0) return;

    version->dwOSVersionInfoSize = sizeof(*version);
#pragma warning(suppress: WARN_DEPRECATED)
    GetVersionExW((OSVERSIONINFOW *)version);
}

static BOOL is_admin_member(void)
{
    SID_IDENTIFIER_AUTHORITY nt_authority = SECURITY_NT_AUTHORITY;
    PSID admins = NULL;
    BOOL member = FALSE;

    if (!AllocateAndInitializeSid(&nt_authority, ADMINS_SID_SUBAUTHORITY_COUNT, SECURITY_BUILTIN_DOMAIN_RID,
                                  DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &admins))
    {
        return FALSE;
    }

    if (!CheckTokenMembership(NULL, admins, &member)) member = FALSE;

    FreeSid(admins);

    return member;
}

void os_get(OSINFO *os_info)
{
    OSVERSIONINFOEXW version;
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    PFN_IsWow64Process is_wow64_process = NULL;
    PFN_GetUserDefaultUILanguage get_ui_language = NULL;

    ZeroMemory(os_info, sizeof(*os_info));
    read_version(&version);

    os_info->major = version.dwMajorVersion;
    os_info->minor = version.dwMinorVersion;
    os_info->build = version.dwBuildNumber;
    os_info->service_pack_major = version.wServicePackMajor;
    lstrcpynW(os_info->service_pack, version.szCSDVersion, OS_SERVICE_PACK_CCH);
    os_info->is_server = (version.wProductType != VER_NT_WORKSTATION);

    if (kernel32)
    {
        is_wow64_process = (PFN_IsWow64Process)GetProcAddress(kernel32, "IsWow64Process");
        get_ui_language = (PFN_GetUserDefaultUILanguage)GetProcAddress(kernel32, "GetUserDefaultUILanguage");
    }

#ifdef _WIN64
    (void)is_wow64_process;
    os_info->is_64bit_os = TRUE;
    os_info->is_wow64 = FALSE;
#else
    BOOL wow64 = FALSE;

    if (is_wow64_process && is_wow64_process(GetCurrentProcess(), &wow64) && wow64) os_info->is_wow64 = TRUE;

    os_info->is_64bit_os = os_info->is_wow64;
#endif

    os_info->ui_language = get_ui_language ? get_ui_language() : GetUserDefaultLangID();
    os_info->is_admin = is_admin_member();
}

void os_log(const OSINFO *os_info)
{
    WCHAR computer[MAX_COMPUTERNAME_LENGTH + 1];
    WCHAR user[UNLEN + 1];
    DWORD computer_cch = ARRAY_COUNT(computer);
    DWORD user_cch = ARRAY_COUNT(user);

    if (!GetComputerNameW(computer, &computer_cch)) lstrcpyW(computer, L"?");
    if (!GetUserNameW(user, &user_cch)) lstrcpyW(user, L"?");

    log_msg(L"Компьютер: %s, пользователь: %s", computer, user);
    log_msg(L"ОС: %lu.%lu.%lu %s%s, %s, язык интерфейса 0x%04X",
            os_info->major, os_info->minor, os_info->build, os_info->service_pack,
            os_info->is_server ? L" (сервер)" : L"",
            os_info->is_64bit_os ? L"64 бит" : L"32 бит", os_info->ui_language);

#ifdef _WIN64
    log_msg(L"Сборка утилиты: x64");
#else
    log_msg(L"Сборка утилиты: x86%s", os_info->is_wow64 ? L" (WOW64 - нужна сборка x64!)" : L"");
#endif

    log_msg(L"Права администратора: %s", os_info->is_admin ? L"да" : L"нет");
    log_msg(L"Экран: %dx%d, виртуальный экран %d,%d %dx%d, мониторов %d, удалённый сеанс: %s",
            GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
            GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
            GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN),
            GetSystemMetrics(SM_CMONITORS),
            GetSystemMetrics(SM_REMOTESESSION) ? L"да" : L"нет");

    HDC screen_dc = GetDC(NULL);

    if (screen_dc)
    {
        log_msg(L"DPI: %d", GetDeviceCaps(screen_dc, LOGPIXELSX));
        ReleaseDC(NULL, screen_dc);
    }
}

void os_set_dpi_aware(void)
{
    HMODULE user32 = GetModuleHandleW(L"user32.dll");

    if (!user32) return;

    // Windows 10 1703+: сначала PER_MONITOR_AWARE_V2, затем PER_MONITOR_AWARE
    PFN_SetProcessDpiAwarenessContext set_context =
        (PFN_SetProcessDpiAwarenessContext)GetProcAddress(user32, "SetProcessDpiAwarenessContext");

    if (set_context)
    {
        if (set_context(DPI_CONTEXT_PER_MONITOR_AWARE_V2) || set_context(DPI_CONTEXT_PER_MONITOR_AWARE)) return;
        if (GetLastError() == ERROR_ACCESS_DENIED) return;      // уже задано манифестом
    }

    // Windows 8.1: shcore!SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE)
    HMODULE shcore = LoadLibraryW(L"shcore.dll");

    if (shcore)
    {
        PFN_SetProcessDpiAwareness set_awareness =
            (PFN_SetProcessDpiAwareness)GetProcAddress(shcore, "SetProcessDpiAwareness");

        if (set_awareness)
        {
            HRESULT result = set_awareness(PROCESS_PER_MONITOR_DPI_AWARE_VALUE);

            if (SUCCEEDED(result) || result == E_ACCESSDENIED) return;
        }
    }

    // Vista/7
    PFN_SetProcessDPIAware set_dpi_aware = (PFN_SetProcessDPIAware)GetProcAddress(user32, "SetProcessDPIAware");

    if (set_dpi_aware) set_dpi_aware();
}

static BOOL object_name(HANDLE object, WCHAR *name, int name_cch)
{
    DWORD needed = 0;

    name[0] = 0;

    if (!GetUserObjectInformationW(object, UOI_NAME, name, (DWORD)(name_cch * sizeof(WCHAR)), &needed)) return FALSE;

    name[name_cch - 1] = 0;

    return TRUE;
}

BOOL os_can_capture(WCHAR *reason, int reason_cch)
{
    WCHAR name[OBJECT_NAME_CCH];
    WCHAR error_text[TEXT_BUFFER_CCH];

    reason[0] = 0;

    HWINSTA station = GetProcessWindowStation();

    if (!station || !object_name(station, name, OBJECT_NAME_CCH))
    {
        format_text(reason, reason_cch, L"не удалось узнать станцию окон: %s",
                    log_errtext(GetLastError(), error_text, TEXT_BUFFER_CCH));
        return FALSE;
    }

    if (lstrcmpiW(name, INTERACTIVE_WINDOW_STATION) != 0)
    {
        format_text(reason, reason_cch, L"процесс не на интерактивной станции окон (станция \"%s\", нужна WinSta0) - "
                    L"запуск из службы или планировщика без входа пользователя", name);
        return FALSE;
    }

    HDESK input_desktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);

    if (!input_desktop)
    {
        format_text(reason, reason_cch, L"нет доступа к входному рабочему столу (экран заблокирован, "
                    L"идёт вход в систему или заставка): %s", log_errtext(GetLastError(), error_text, TEXT_BUFFER_CCH));
        return FALSE;
    }

    if (!object_name(input_desktop, name, OBJECT_NAME_CCH))
    {
        format_text(reason, reason_cch, L"не удалось узнать имя входного стола: %s",
                    log_errtext(GetLastError(), error_text, TEXT_BUFFER_CCH));
        CloseDesktop(input_desktop);
        return FALSE;
    }

    CloseDesktop(input_desktop);

    if (lstrcmpiW(name, DEFAULT_DESKTOP) != 0)
    {
        format_text(reason, reason_cch, L"активен рабочий стол \"%s\", а не Default "
                    L"(экран заблокирован, окно UAC или заставка)", name);
        return FALSE;
    }

    // Наш поток должен быть на том же столе, что и входной
    HDESK thread_desktop = GetThreadDesktop(GetCurrentThreadId());

    if (thread_desktop && object_name(thread_desktop, name, OBJECT_NAME_CCH) && lstrcmpiW(name, DEFAULT_DESKTOP) != 0)
    {
        format_text(reason, reason_cch, L"процесс работает на рабочем столе \"%s\", а не Default", name);
        return FALSE;
    }

    return TRUE;
}
