#include "scenarios.h"
#include "capture.h"
#include "common.h"
#include "console.h"
#include "log.h"
#include "mmc.h"
#include "scroll.h"
#include "wnd.h"
#include <commctrl.h>   // PSM_SETCURSEL, PSM_GETTABCONTROL, TCM_GETITEMCOUNT - только константы

// Снимок
#define CLOCK_TIP_TIMEOUT_MS   3000     // сколько ждать подсказку у часов перед снимком
#define REDRAW_WAIT_MS         500      // окна только что подняты наверх - дать им перерисоваться
#define BALLOON_REDRAW_WAIT_MS 200      // спрятан новый шарик - дать перерисовать место под ним

// Окна свойств
#define SHEET_MESSAGE_TIMEOUT_MS 3000   // сообщение окну свойств: не ответило - считаем зависшим
#define TAB_SWITCH_WAIT_MS       300    // вкладка переключилась - дать ей нарисоваться
#define SYSDM_FIRST_TAB          0      // "Свойства системы": XP - "Общие", Vista+ - "Имя компьютера"
#define TIMEDATE_INTERNET_TAB    2      // "Дата и время" на XP: "Время Интернета" - третья вкладка
#define TIMEDATE_MIN_TABS        3      // меньше вкладок - "Времени Интернета" нет (компьютер в домене)
#define TABS_WAIT_MS             3000   // вкладки могут добавляться уже после появления окна
#define TABS_POLL_MS             200
#define TABS_LATE_LOG_MS         250    // третья вкладка появилась позже - записать в журнал

// Ожидание окон
#define DIALOG_OPEN_TIMEOUT_MS   20000
#define SYSTEM_OPEN_TIMEOUT_MS   15000  // панель "Система"
#define SETTINGS_OPEN_TIMEOUT_MS 20000  // Параметры (без прав администратора) открываются дольше
#define SOFTWARE_OPEN_TIMEOUT_MS 30000
#define CONSOLE_OPEN_TIMEOUT_MS  15000
#define COMMAND_TIMEOUT_MS       30000  // лимит на одну команду в cmd
#define SOFTWARE_STABLE_MS       2500   // список программ не меняется столько - заполнен

#define EXPLORER_CLASS L"CabinetWClass"
#define SETTINGS_CLASS L"ApplicationFrameWindow"    // Параметры Windows 10/11 (приложение UWP)
#define CONSOLE_CLASS  L"ConsoleWindowClass"

#define SYNC_MAX_WINDOWS 3              // сведения о системе + cmd + "Дата и время"

// Окна сценария "Установленное ПО" в порядке подъёма наверх (последнее - самое верхнее)
enum
{
    SOFTWARE_SYSINFO_WINDOW,
    SOFTWARE_LIST_WINDOW,
    SOFTWARE_WINDOW_COUNT
};

static const WCHAR *const DIALOG_CLASSES[] = { WND_DIALOG_CLASS, NULL };
static const WCHAR *const SYSTEM_CLASSES[] = { EXPLORER_CLASS, SETTINGS_CLASS, WND_DIALOG_CLASS, NULL };
static const WCHAR *const EXPLORER_CLASSES[] = { EXPLORER_CLASS, NULL };
static const WCHAR *const CONSOLE_CLASSES[] = { CONSOLE_CLASS, NULL };

// Страница списка ПО: имя файла по шаблону сценария ("..._%d.png")
typedef struct
{
    const SCEN_CONTEXT *context;
    const SCENARIO     *scenario;
    HWND               *windows;        // SOFTWARE_WINDOW_COUNT окон
} SOFTWARE_PAGE;

static HWND g_sysinfo = NULL;
static BOOL g_sysinfo_tried = FALSE;

// Общие шаги

BOOL scen_shoot(const SCEN_CONTEXT *context, const WCHAR *file_name, HWND *windows, int count)
{
    WCHAR path[PATH_BUFFER_CCH];
    WCHAR error_text[MESSAGE_BUFFER_CCH];

    if (wnd_aborting()) return FALSE;

    format_text(path, PATH_BUFFER_CCH, L"%s\\%s", context->results_dir, file_name);

    if (count > 0) wnd_raise(windows, count);

    wnd_clock_tip_show(CLOCK_TIP_TIMEOUT_MS);
    Sleep(REDRAW_WAIT_MS);

    // новый шарик мог вылезти, пока ждали
    if (wnd_hide_tray_balloons()) Sleep(BALLOON_REDRAW_WAIT_MS);

    BOOL ok = cap_screen_png(path, context->all_monitors, error_text, MESSAGE_BUFFER_CCH);

    wnd_clock_tip_hide();

    if (count > 0) wnd_unraise(windows, count);

    if (ok) log_msg(L"Снимок сохранён: %s", file_name);
    else log_msg(L"ОШИБКА снимка %s: %s", file_name, error_text);

    return ok;
}

// Путь к программе из system32: "C:\Windows\system32\<exe>" в кавычках + аргументы.
static void system_command(WCHAR *command, int command_cch, const WCHAR *exe, const WCHAR *args)
{
    WCHAR system_dir[MAX_PATH];

    if (!GetSystemDirectoryW(system_dir, MAX_PATH)) lstrcpyW(system_dir, L"C:\\Windows\\system32");

    format_text(command, command_cch, L"\"%s\\%s\" %s", system_dir, exe, args);
}

// Переключить вкладку окна свойств (property sheet) по номеру.
static void select_tab(HWND sheet, int index)
{
    DWORD_PTR result = 0;

    if (!SendMessageTimeoutW(sheet, PSM_SETCURSEL, (WPARAM)index, 0, SMTO_ABORTIFHUNG, SHEET_MESSAGE_TIMEOUT_MS,
                             &result))
    {
        log_msg(L"Не удалось переключить вкладку %d", index);
    }
}

// "Свойства системы", первая вкладка: XP - "Общие" (версия ОС), Vista+ - "Имя компьютера".
// На Win10 22H2 "rundll32 sysdm.cpl" и "control sysdm.cpl" молча ничего не открывают
// (проверено), поэтому на 6.x+ - SystemPropertiesComputerName.exe (есть с Vista).
static HWND open_system_properties(const OSINFO *os_info)
{
    WCHAR command[PATH_BUFFER_CCH];

    if (os_info->major < OS_MAJOR_VISTA)
    {
        system_command(command, PATH_BUFFER_CCH, L"rundll32.exe", L"shell32.dll,Control_RunDLL sysdm.cpl,,0");
    }
    else
    {
        system_command(command, PATH_BUFFER_CCH, L"SystemPropertiesComputerName.exe", L"");
    }

    HWND window = wnd_open(command, DIALOG_CLASSES, DIALOG_OPEN_TIMEOUT_MS, L"Свойства системы");

    if (window)
    {
        select_tab(window, SYSDM_FIRST_TAB);
        Sleep(TAB_SWITCH_WAIT_MS);
    }

    return window;
}

// Vista+: панель "Система" (на Win10 20H2+ - сразу "Свойства системы"); без прав
// администратора "Свойства системы" не открыть - тогда Параметры.
static HWND open_sysinfo_nt6(const OSINFO *os_info)
{
    WCHAR command[PATH_BUFFER_CCH];
    WCHAR class_name[CLASS_NAME_CCH];
    HWND window = NULL;
    BOOL settings_instead_of_panel =
        os_info->major > OS_MAJOR_WIN10 || (os_info->major == OS_MAJOR_WIN10 && os_info->build >= OS_BUILD_WIN10_20H2);

    system_command(command, PATH_BUFFER_CCH, L"control.exe", L"/name Microsoft.System");

    if (settings_instead_of_panel)
    {
        // Win10 20H2+ и Win11: "Система" открывает Параметры, где версия ОС ниже края окна
        log_msg(L"Сведения о системе: сборка %lu - панель \"Система\" заменена Параметрами, беру \"Свойства системы\"",
                os_info->build);
    }
    else
    {
        window = wnd_open(command, SYSTEM_CLASSES, SYSTEM_OPEN_TIMEOUT_MS, L"Система");

        if (window && GetClassNameW(window, class_name, CLASS_NAME_CCH) && lstrcmpiW(class_name, SETTINGS_CLASS) == 0)
        {
            log_msg(L"Сведения о системе: открылись Параметры, закрываю и беру \"Свойства системы\"");
            wnd_close(window);
            window = NULL;
        }
    }

    if (window) return window;

    window = open_system_properties(os_info);

    if (!window && GetLastError() == ERROR_ELEVATION_REQUIRED)
    {
        // без прав администратора SystemPropertiesComputerName не запускается -
        // берём Параметры / панель "Система": там тоже видно имя компьютера
        log_msg(L"Сведения о системе: нет прав администратора, открываю \"Система\" (Параметры)");
        window = wnd_open(command, SYSTEM_CLASSES, SETTINGS_OPEN_TIMEOUT_MS, L"Система");
    }

    return window;
}

HWND scen_sysinfo(const SCEN_CONTEXT *context)
{
    const OSINFO *os_info = context->os_info;

    if (g_sysinfo && IsWindow(g_sysinfo)) return g_sysinfo;
    if (g_sysinfo_tried) return NULL;       // уже не получилось - не пытаемся в каждом сценарии

    g_sysinfo_tried = TRUE;

    // XP / 2003: "Свойства системы", вкладка "Общие"
    HWND window = os_info->major < OS_MAJOR_VISTA ? open_system_properties(os_info) : open_sysinfo_nt6(os_info);

    if (!window) log_msg(L"Окно сведений о системе открыть не удалось - снимки будут без него");

    g_sysinfo = window;

    return window;
}

// Сценарии

// Служебный сценарий этапа 1: только подсказка у часов и снимок экрана.
static BOOL run_test(const SCEN_CONTEXT *context, const SCENARIO *scenario)
{
    return scen_shoot(context, scenario->file_name, NULL, 0);
}

static BOOL shoot_software_page(int page, int page_count, void *arg)
{
    SOFTWARE_PAGE *software_page = (SOFTWARE_PAGE *)arg;
    WCHAR file_name[MAX_PATH];

    format_text(file_name, MAX_PATH, software_page->scenario->file_name, page);

    if (page_count > 1) log_msg(L"Страница %d из %d", page, page_count);

    return scen_shoot(software_page->context, file_name, software_page->windows, SOFTWARE_WINDOW_COUNT);
}

// Установленное ПО: appwiz.cpl + сведения о системе; длинный список - постранично.
static BOOL run_installed_software(const SCEN_CONTEXT *context, const SCENARIO *scenario)
{
    WCHAR command[PATH_BUFFER_CCH];
    HWND windows[SOFTWARE_WINDOW_COUNT];
    HWND list_window;
    SOFTWARE_PAGE software_page;
    int page_count = 1;

    HWND sysinfo = scen_sysinfo(context);

    if (context->os_info->major < OS_MAJOR_VISTA)
    {
        // XP: окно "Установка и удаление программ" живёт в процессе rundll32; класс заранее не знаем
        system_command(command, PATH_BUFFER_CCH, L"rundll32.exe", L"shell32.dll,Control_RunDLL appwiz.cpl");
        list_window = wnd_open(command, NULL, SOFTWARE_OPEN_TIMEOUT_MS, L"Установка и удаление программ");
    }
    else
    {
        system_command(command, PATH_BUFFER_CCH, L"control.exe", L"appwiz.cpl");
        list_window = wnd_open(command, EXPLORER_CLASSES, SOFTWARE_OPEN_TIMEOUT_MS, L"Программы и компоненты");
    }

    if (!list_window) return FALSE;

    wnd_layout(list_window, sysinfo, NULL);
    windows[SOFTWARE_SYSINFO_WINDOW] = sysinfo;
    windows[SOFTWARE_LIST_WINDOW] = list_window;
    wnd_raise(windows, SOFTWARE_WINDOW_COUNT);

    // XP долго считает размеры программ - ждём, пока окно и его процесс успокоятся
    if (!wnd_wait_ready(list_window, SOFTWARE_STABLE_MS, context->wait_ms))
    {
        log_msg(L"Внимание: список программ, возможно, заполнен не полностью");
    }

    if (context->dump) scroll_set_debug_dir(context->results_dir);

    software_page.context = context;
    software_page.scenario = scenario;
    software_page.windows = windows;

    int shots_ok = scroll_pages(list_window, context->max_pages, context->dump, shoot_software_page, &software_page,
                                &page_count);
    BOOL ok = (shots_ok == page_count);

    if (!ok) log_msg(L"Снято страниц: %d из %d", shots_ok, page_count);
    if (!context->keep) wnd_close(list_window);

    return ok;
}

// Синхронизация времени

// Диалог - под окном cmd, правым краем к окну сведений о системе (как на ручных примерах XP:
// cmd сверху слева, "Дата и время" под ним, "Свойства системы" справа). Не влезает по высоте -
// прижать к низу рабочей области (перекроет низ cmd, где уже пусто).
static void place_below(HWND dialog, HWND console, HWND sysinfo)
{
    RECT work_area, dialog_rect, console_rect, sysinfo_rect;

    wnd_work_area(&work_area);

    if (!GetWindowRect(dialog, &dialog_rect) || !GetWindowRect(console, &console_rect)) return;

    int width = dialog_rect.right - dialog_rect.left;
    int height = dialog_rect.bottom - dialog_rect.top;
    int right = console_rect.right;

    if (sysinfo && IsWindow(sysinfo) && GetWindowRect(sysinfo, &sysinfo_rect) && sysinfo_rect.left < right)
    {
        right = sysinfo_rect.left;
    }

    int x = right - width;

    if (x < work_area.left) x = work_area.left;

    int y = console_rect.bottom;

    if (y + height > work_area.bottom) y = work_area.bottom - height;
    if (y < work_area.top) y = work_area.top;

    SetWindowPos(dialog, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    log_msg(L"Раскладка: \"Дата и время\" %d,%d %dx%d", x, y, width, height);
}

// Число вкладок окна свойств: вкладки могут добавляться уже после появления окна - ждём
// TIMEDATE_MIN_TABS вкладок не дольше TABS_WAIT_MS от start_tick.
static int wait_tab_count(HWND tab_control, DWORD start_tick)
{
    for (;;)
    {
        DWORD_PTR result = 0;

        SendMessageTimeoutW(tab_control, TCM_GETITEMCOUNT, 0, 0, SMTO_ABORTIFHUNG, SHEET_MESSAGE_TIMEOUT_MS, &result);

        int tab_count = (int)result;

        if (tab_count >= TIMEDATE_MIN_TABS || wnd_aborting() || GetTickCount() - start_tick >= TABS_WAIT_MS)
        {
            return tab_count;
        }

        Sleep(TABS_POLL_MS);
    }
}

// XP/2003: "Дата и время" на вкладке "Время Интернета" (третья). В домене этой вкладки нет
// (синхронизация идёт с контроллера домена) - тогда окно закрываем и снимаем без него. Номер
// вкладки, а не название: не зависит от языка. NULL - окна не будет.
static HWND open_date_time(const SCEN_CONTEXT *context, HWND console, HWND sysinfo)
{
    WCHAR command[PATH_BUFFER_CCH];
    DWORD_PTR result = 0;
    HWND tab_control = NULL;

    system_command(command, PATH_BUFFER_CCH, L"rundll32.exe", L"shell32.dll,Control_RunDLL timedate.cpl");

    HWND dialog = wnd_open(command, DIALOG_CLASSES, DIALOG_OPEN_TIMEOUT_MS, L"Дата и время");

    if (!dialog) return NULL;

    if (SendMessageTimeoutW(dialog, PSM_GETTABCONTROL, 0, 0, SMTO_ABORTIFHUNG, SHEET_MESSAGE_TIMEOUT_MS, &result))
    {
        tab_control = (HWND)result;
    }

    if (!tab_control || !IsWindow(tab_control))
    {
        // например, сообщение "недостаточно прав для изменения времени" вместо окна свойств
        log_msg(L"\"Дата и время\": открылось не окно свойств (нет вкладок) - закрываю, снимаю без него");
        wnd_close(dialog);
        return NULL;
    }

    DWORD start_tick = GetTickCount();
    int tab_count = wait_tab_count(tab_control, start_tick);

    if (tab_count < TIMEDATE_MIN_TABS)
    {
        log_msg(L"\"Дата и время\": вкладок %d - \"Время Интернета\" нет (компьютер в домене?), закрываю, снимаю "
                L"без него", tab_count);

        if (context->dump) scen_shoot(context, L"_дата_и_время.png", &dialog, 1);

        wnd_close(dialog);
        return NULL;
    }

    DWORD waited_ms = GetTickCount() - start_tick;

    if (waited_ms > TABS_LATE_LOG_MS) log_msg(L"\"Дата и время\": третья вкладка появилась через %lu мс", waited_ms);

    select_tab(dialog, TIMEDATE_INTERNET_TAB);
    log_msg(L"\"Дата и время\": вкладок %d, выбрана третья (\"Время Интернета\")", tab_count);
    Sleep(TAB_SWITCH_WAIT_MS);
    place_below(dialog, console, sysinfo);

    return dialog;
}

// Открыть cmd на месте целевого окна: rect - назначенный ему прямоугольник (заполняется здесь).
static HWND open_console(HWND sysinfo, RECT *rect, DWORD *process_id)
{
    WCHAR command[PATH_BUFFER_CCH];
    WCHAR home[MAX_PATH];
    STARTUPINFOW startup_info;

    // место под cmd известно заранее: ширину буфера задаём при запуске, чтобы шапка и вывод
    // сразу переносились по ширине окна (на XP/7 сужение буфера потом обрезает строки)
    wnd_layout(NULL, sysinfo, rect);
    ZeroMemory(&startup_info, sizeof(startup_info));
    startup_info.cb = sizeof(startup_info);
    startup_info.dwFlags = STARTF_USEPOSITION;
    startup_info.dwX = (DWORD)rect->left;
    startup_info.dwY = (DWORD)rect->top;

    if (!con_startup_size(rect->right - rect->left, rect->bottom - rect->top, &startup_info))
    {
        log_msg(L"Консоль: размер шрифта не узнать (своей консоли нет) - ширина подгоняется после запуска");
    }

    // cmd - обычным интерактивным окном (с шапкой "Microsoft Windows..."), папка - профиль
    // пользователя, как при запуске через "Выполнить"; новая консоль - иначе cmd сядет в нашу
    system_command(command, PATH_BUFFER_CCH, L"cmd.exe", L"");

    DWORD home_length = GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH);

    if (home_length == 0 || home_length >= MAX_PATH || GetFileAttributesW(home) == INVALID_FILE_ATTRIBUTES)
    {
        home[0] = 0;
    }

    return wnd_open_ex(command, home[0] ? home : NULL, CREATE_NEW_CONSOLE, &startup_info, CONSOLE_CLASSES,
                       CONSOLE_OPEN_TIMEOUT_MS, L"Командная строка", process_id);
}

// Вернуть окно cmd в левый верхний угол прямоугольника rect (после смены размера консоль
// могла сдвинуться).
static void place_console(HWND console, const RECT *rect)
{
    RECT console_rect;

    SetWindowPos(console, NULL, rect->left, rect->top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    GetWindowRect(console, &console_rect);
    log_msg(L"Раскладка: командная строка %d,%d %dx%d", console_rect.left, console_rect.top,
            console_rect.right - console_rect.left, console_rect.bottom - console_rect.top);
}

// Синхронизация времени: cmd, в котором команды "набраны с клавиатуры" (видно
// "приглашение>команда" и вывод), + на XP "Дата и время", + сведения о системе.
static BOOL run_time_sync(const SCEN_CONTEXT *context, const SCENARIO *scenario)
{
    static const WCHAR *const COMMANDS_XP[] = { L"net time /querysntp" };
    static const WCHAR *const COMMANDS_NT6[] =
    {
        L"w32tm /query /source",
        L"w32tm /query /status",
        L"w32tm /query /peers"
    };
    const BOOL xp = context->os_info->major < OS_MAJOR_VISTA;
    HWND windows[SYNC_MAX_WINDOWS];
    HWND dialog = NULL;
    RECT console_rect;
    DWORD console_process_id = 0;
    int window_count = 0;

    HWND sysinfo = scen_sysinfo(context);
    HWND console = open_console(sysinfo, &console_rect, &console_process_id);

    if (!console) return FALSE;

    wnd_layout(console, sysinfo, &console_rect);

    // на XP ниже cmd встанет "Дата и время" - окно cmd уменьшить по содержимому
    const WCHAR *const *commands = xp ? COMMANDS_XP : COMMANDS_NT6;
    int command_count = xp ? (int)ARRAY_COUNT(COMMANDS_XP) : (int)ARRAY_COUNT(COMMANDS_NT6);
    BOOL typed = con_type_commands(console_process_id, console_rect.right - console_rect.left,
                                   console_rect.bottom - console_rect.top, xp, commands, command_count,
                                   COMMAND_TIMEOUT_MS);

    if (IsWindow(console)) place_console(console, &console_rect);
    if (xp && !wnd_aborting()) dialog = open_date_time(context, console, sysinfo);

    windows[window_count++] = sysinfo;
    windows[window_count++] = console;

    if (dialog) windows[window_count++] = dialog;

    BOOL ok = scen_shoot(context, scenario->file_name, windows, window_count);

    if (!context->keep)
    {
        if (dialog) wnd_close(dialog);

        wnd_close(console);
    }

    if (!typed) log_msg(L"Внимание: команды в cmd введены или выполнены не полностью");

    return ok && typed;
}

// gpedit.msc (MMC)

#define GPEDIT_OPEN_TIMEOUT_MS   30000  // mmc.exe на XP запускается медленно
#define MMC_ATTACH_TIMEOUT_MS    15000  // появление дерева и корневого узла
#define MMC_EXPAND_TIMEOUT_MS    5000   // раскрытие одного узла (ленивая загрузка узлов MMC)
#define MMC_LIST_TIMEOUT_MS      15000  // заполнение правой панели

// Окна сценариев gpedit в порядке подъёма наверх (последнее - самое верхнее)
enum
{
    GPEDIT_SYSINFO_WINDOW,
    GPEDIT_WINDOW,
    GPEDIT_WINDOW_COUNT
};

// Открыть gpedit, пройти путь к узлу политики и снять экран. Снимок делается, только когда узел
// выбран и правая панель заполнена; иначе (узел не найден) сценарий пропускается.
static BOOL run_gpedit_policy(const SCEN_CONTEXT *context, const SCENARIO *scenario, const NAMES_PATH *path)
{
    HWND windows[GPEDIT_WINDOW_COUNT];
    MMC mmc;
    BOOL ok = FALSE;

    HWND sysinfo = scen_sysinfo(context);
    HWND frame = mmc_open_console(L"gpedit", scenario->title, GPEDIT_OPEN_TIMEOUT_MS);

    if (!frame) return FALSE;

    wnd_layout(frame, sysinfo, NULL);

    if (mmc_attach(&mmc, frame, MMC_ATTACH_TIMEOUT_MS))
    {
        if (mmc_select_path(&mmc, path, MMC_EXPAND_TIMEOUT_MS))
        {
            mmc_show_result_list(&mmc, MMC_LIST_TIMEOUT_MS);

            if (context->dump) mmc_dump(&mmc);

            windows[GPEDIT_SYSINFO_WINDOW] = sysinfo;
            windows[GPEDIT_WINDOW] = frame;
            ok = scen_shoot(context, scenario->file_name, windows, GPEDIT_WINDOW_COUNT);
        }
        else if (context->dump)
        {
            mmc_dump(&mmc);
        }
    }

    mmc_detach(&mmc);

    if (!context->keep) wnd_close(frame);

    return ok;
}

static BOOL run_audit_policy(const SCEN_CONTEXT *context, const SCENARIO *scenario)
{
    return run_gpedit_policy(context, scenario, &g_names_audit_policy);
}

static BOOL run_password_policy(const SCEN_CONTEXT *context, const SCENARIO *scenario)
{
    return run_gpedit_policy(context, scenario, &g_names_password_policy);
}

static BOOL run_lockout_policy(const SCEN_CONTEXT *context, const SCENARIO *scenario)
{
    return run_gpedit_policy(context, scenario, &g_names_lockout_policy);
}

// Имена файлов - как в ручных скриншотах NEVSKA03. Список ПО - постранично:
// "Установленное ПО_1.png", "_2" и т. д. (%d - номер страницы).
// Поля: имя для /only, имя файла, описание, служебный (только по /only), функция сценария.
const SCENARIO g_scenarios[] =
{
    { L"Ustanovlennoe_PO", L"Установленное ПО_%d.png",
      L"Установленное ПО (appwiz.cpl), постранично", FALSE, run_installed_software },
    { L"audit_base", L"Аудит.png",
      L"gpedit: Локальные политики - Политика аудита", FALSE, run_audit_policy },
    { L"password_policy", L"Политика паролей.png",
      L"gpedit: Политики учётных записей - Политика паролей", FALSE, run_password_policy },
    { L"acc_policy1", L"Политика блокировки.png",
      L"gpedit: Политика блокировки учётной записи", FALSE, run_lockout_policy },
    { L"sync_time", L"sync_time.png",
      L"Синхронизация времени: cmd (w32tm / net time), XP - \"Дата и время\"", FALSE, run_time_sync },
    { L"users_list", L"user_list.png",
      L"lusrmgr: Пользователи", FALSE, NULL },
    { L"users_group_admin", L"user_list_admin.png",
      L"lusrmgr: свойства группы S-1-5-32-544", FALSE, NULL },
    { L"users_group_user", L"user_list_users.png",
      L"lusrmgr: свойства группы S-1-5-32-545", FALSE, NULL },
    { L"test", L"test.png",
      L"Проверка: подсказка у часов + снимок экрана", TRUE, run_test },
};

const int g_scenario_count = (int)ARRAY_COUNT(g_scenarios);
