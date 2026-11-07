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
    if (count > 0 && context->dump) wnd_log_z_order(windows, count);

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
            mmc_wait_result_list(&mmc, MMC_LIST_TIMEOUT_MS);
            mmc_fit_value_column(&mmc);

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

// lusrmgr.msc (MMC): пользователи и свойства групп - одним снимком

#define LUSRMGR_OPEN_TIMEOUT_MS          30000  // mmc.exe на XP запускается медленно
#define PROPERTIES_OPEN_TIMEOUT_MS       10000  // окно свойств группы после Enter (и столько же - запасной путь)
#define PROPERTIES_STABLE_MS             1000   // окно свойств не меняется столько - члены группы показаны
#define PROPERTIES_READY_TIMEOUT_MS      10000  // имена членов из недоступного домена определяются долго
#define USERS_TEXT_COLUMNS               2      // список пользователей: "Имя" и "Полное имя" - целиком
#define USERS_CONSOLE_MIN_HEIGHT_PERCENT 45     // консоль внизу - не ниже 45% высоты рабочей области

// Окна сценария "Пользователи" в порядке подъёма наверх (последнее - самое верхнее), как в образце:
// консоль поверх нижней части окон свойств
enum
{
    USERS_SYSINFO_WINDOW,
    USERS_ADMINS_WINDOW,
    USERS_USERS_WINDOW,
    USERS_CONSOLE_WINDOW,
    USERS_WINDOW_COUNT
};

// Встроенная группа, свойства которой открываются. Имя на каждом языке своё - узнаём по SID.
typedef struct
{
    WELL_KNOWN_SID_TYPE sid_type;
    const WCHAR        *sid_text;   // для журнала
    int                 window;     // место окна свойств в массиве окон снимка
} USERS_GROUP;

static const USERS_GROUP USERS_GROUPS[] =
{
    { WinBuiltinAdministratorsSid, L"S-1-5-32-544", USERS_ADMINS_WINDOW },
    { WinBuiltinUsersSid,          L"S-1-5-32-545", USERS_USERS_WINDOW },
};

// Имя встроенной группы на языке системы ("Администраторы" / "Administrators"). FALSE - не узнать.
static BOOL group_name(const USERS_GROUP *group, WCHAR *name, DWORD name_cch)
{
    BYTE sid[SECURITY_MAX_SID_SIZE];
    DWORD sid_size = sizeof(sid);
    WCHAR domain[TEXT_BUFFER_CCH];
    DWORD domain_cch = TEXT_BUFFER_CCH;
    SID_NAME_USE sid_use;
    WCHAR error_text[TEXT_BUFFER_CCH];

    name[0] = 0;

    if (!CreateWellKnownSid(group->sid_type, NULL, sid, &sid_size) ||
        !LookupAccountSidW(NULL, sid, name, &name_cch, domain, &domain_cch, &sid_use))
    {
        log_msg(L"Группа %s: имя не узнать: %s", group->sid_text,
                log_errtext(GetLastError(), error_text, TEXT_BUFFER_CCH));
        return FALSE;
    }

    log_msg(L"Группа %s: \"%s\\%s\"", group->sid_text, domain, name);

    return TRUE;
}

// /dump: окно свойств не успокоилось - кадр экрана "_свойства_<SID>.png" для разбора.
static void dump_properties_frame(const SCEN_CONTEXT *context, const USERS_GROUP *group)
{
    WCHAR path[PATH_BUFFER_CCH];
    WCHAR error_text[MESSAGE_BUFFER_CCH];

    format_text(path, PATH_BUFFER_CCH, L"%s\\_свойства_%s.png", context->results_dir, group->sid_text);

    if (!cap_screen_png(path, context->all_monitors, error_text, MESSAGE_BUFFER_CCH))
    {
        log_msg(L"Кадр окна свойств не сохранён: %s", error_text);
    }
}

// Открыть свойства группы из списка групп (узел "Группы" уже выбран) и дождаться, пока окно
// заполнится. NULL - не открылось.
static HWND open_group_properties(const SCEN_CONTEXT *context, MMC *mmc, const USERS_GROUP *group)
{
    WCHAR name[TEXT_BUFFER_CCH];
    WCHAR log_name[TEXT_BUFFER_CCH];

    if (!group_name(group, name, TEXT_BUFFER_CCH)) return NULL;

    format_text(log_name, TEXT_BUFFER_CCH, L"Свойства группы %s", name);

    HWND dialog = mmc_open_item_properties(mmc, name, log_name, PROPERTIES_OPEN_TIMEOUT_MS);

    if (!dialog) return NULL;

    // готовность - по картинке списка членов группы (не всего окна: на Win7 у активного окна плавно
    // мигает кнопка OK, и картинка окна не успокаивается); на время ожидания окно поверх всех
    HWND members = wnd_find_descendant(dialog, L"SysListView32");

    wnd_raise(&dialog, 1);

    if (!wnd_wait_ready(members ? members : dialog, PROPERTIES_STABLE_MS, PROPERTIES_READY_TIMEOUT_MS))
    {
        log_msg(L"Внимание: окно свойств группы %s, возможно, заполнено не полностью", name);

        if (context->dump) dump_properties_frame(context, group);
    }

    wnd_unraise(&dialog, 1);

    return dialog;
}

// Сдвинуть окно свойств (размер у него постоянный) и записать в журнал.
static void place_properties(HWND dialog, int x, int y)
{
    RECT rect;

    SetWindowPos(dialog, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    GetWindowRect(dialog, &rect);
    log_msg(L"Раскладка: окно свойств группы %d,%d %dx%d", rect.left, rect.top, rect.right - rect.left,
            rect.bottom - rect.top);
}

// Низ списка членов группы в окне свойств (экранная координата) - в bottom, если он ниже.
// FALSE - списка нет.
static BOOL lower_members_bottom(HWND dialog, int *bottom)
{
    RECT rect;
    HWND members = dialog ? wnd_find_descendant(dialog, L"SysListView32") : NULL;

    if (!members || !GetWindowRect(members, &rect)) return FALSE;
    if (rect.bottom > *bottom) *bottom = rect.bottom;

    return TRUE;
}

// Раскладка как в образце (VIEWER1\users.PNG): сведения о системе справа (как во всех сценариях);
// окна свойств групп сверху - "Администраторы" у левого края, "Пользователи" рядом, а если места не
// хватает - правым краем к сведениям о системе, поверх правой части "Администраторов"; консоль снизу
// слева, поверх низа окон свойств: верх - по низ списков членов групп, но высота консоли не меньше
// USERS_CONSOLE_MIN_HEIGHT_PERCENT рабочей области (на маленьком экране низ списков закрывается).
static void place_users_windows(HWND *windows)
{
    RECT region, rect;
    HWND admins = windows[USERS_ADMINS_WINDOW];
    HWND users = windows[USERS_USERS_WINDOW];

    // region - место слева от сведений о системе
    wnd_layout(NULL, windows[USERS_SYSINFO_WINDOW], &region);

    int next_x = region.left;

    if (admins && GetWindowRect(admins, &rect))
    {
        place_properties(admins, region.left, region.top);
        next_x = region.left + rect.right - rect.left;
    }

    if (users && GetWindowRect(users, &rect))
    {
        int width = rect.right - rect.left;

        if (next_x + width > region.right) next_x = region.right - width;
        if (next_x < region.left) next_x = region.left;

        place_properties(users, next_x, region.top);
    }

    HWND console = windows[USERS_CONSOLE_WINDOW];
    int region_height = region.bottom - region.top;
    int lowest_top = region.bottom - region_height * USERS_CONSOLE_MIN_HEIGHT_PERCENT / PERCENT_BASE;
    int top = region.top;
    BOOL admins_members = lower_members_bottom(admins, &top);
    BOOL users_members = lower_members_bottom(users, &top);

    if (!admins_members && !users_members) top = region.top + region_height / 2;   // списков нет - с середины
    if (top > lowest_top) top = lowest_top;

    SetWindowPos(console, NULL, region.left, top, region.right - region.left, region.bottom - top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    GetWindowRect(console, &rect);
    log_msg(L"Раскладка: консоль %d,%d %dx%d", rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top);
}

// Открыть свойства обеих групп (узел "Группы"), вернуться к узлу "Пользователи", разложить окна и снять.
// Без списка пользователей снимка нет; без окна свойств группы снимок делается, но сценарий не удался.
static BOOL shoot_users(const SCEN_CONTEXT *context, const SCENARIO *scenario, MMC *mmc, HWND *windows)
{
    // MMC 3.0: область действий отнимает у списка пользователей почти всё место (Win7: 110 px из 300)
    mmc_hide_action_pane(mmc);

    BOOL groups_shown = mmc_select_path(mmc, &g_names_local_groups, MMC_EXPAND_TIMEOUT_MS) &&
                        mmc_wait_result_list(mmc, MMC_LIST_TIMEOUT_MS);
    BOOL groups_opened = groups_shown;

    // группы - независимо друг от друга: не открылась одна - вторую всё равно пробуем
    for (int i = 0; groups_shown && i < (int)ARRAY_COUNT(USERS_GROUPS); i++)
    {
        windows[USERS_GROUPS[i].window] = open_group_properties(context, mmc, &USERS_GROUPS[i]);

        if (!windows[USERS_GROUPS[i].window]) groups_opened = FALSE;
    }

    // окна свойств немодальные: консоль работает дальше, в правой панели - список пользователей
    if (!mmc_select_path(mmc, &g_names_local_users, MMC_EXPAND_TIMEOUT_MS) ||
        !mmc_wait_result_list(mmc, MMC_LIST_TIMEOUT_MS))
    {
        if (context->dump) mmc_dump(mmc);

        return FALSE;
    }

    mmc_fit_text_columns(mmc, USERS_TEXT_COLUMNS);

    if (context->dump) mmc_dump(mmc);

    place_users_windows(windows);

    BOOL shot = scen_shoot(context, scenario->file_name, windows, USERS_WINDOW_COUNT);

    if (!groups_opened) log_msg(L"Внимание: снимок сделан без окна свойств группы - сценарий не удался");

    return shot && groups_opened;
}

// Пользователи и группы: lusrmgr на узле "Пользователи" + свойства групп S-1-5-32-544 и S-1-5-32-545 +
// сведения о системе - все окна на одном снимке.
static BOOL run_users(const SCEN_CONTEXT *context, const SCENARIO *scenario)
{
    HWND windows[USERS_WINDOW_COUNT];
    MMC mmc;
    BOOL ok = FALSE;

    ZeroMemory(windows, sizeof(windows));
    windows[USERS_SYSINFO_WINDOW] = scen_sysinfo(context);
    windows[USERS_CONSOLE_WINDOW] = mmc_open_console(L"lusrmgr", L"Локальные пользователи и группы",
                                                     LUSRMGR_OPEN_TIMEOUT_MS);

    if (!windows[USERS_CONSOLE_WINDOW]) return FALSE;

    wnd_layout(windows[USERS_CONSOLE_WINDOW], windows[USERS_SYSINFO_WINDOW], NULL);

    if (mmc_attach(&mmc, windows[USERS_CONSOLE_WINDOW], MMC_ATTACH_TIMEOUT_MS))
    {
        ok = shoot_users(context, scenario, &mmc, windows);
    }
    else if (context->dump)
    {
        mmc_dump(&mmc);
    }

    mmc_detach(&mmc);

    // окна свойств - раньше консоли: пока они открыты, MMC на закрытие отвечает сообщением (Win7)
    if (!context->keep)
    {
        if (windows[USERS_USERS_WINDOW]) wnd_close(windows[USERS_USERS_WINDOW]);
        if (windows[USERS_ADMINS_WINDOW]) wnd_close(windows[USERS_ADMINS_WINDOW]);

        wnd_close(windows[USERS_CONSOLE_WINDOW]);
    }

    return ok;
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
    { L"users", L"Пользователи.png",
      L"lusrmgr: пользователи + свойства групп S-1-5-32-544 и S-1-5-32-545", FALSE, run_users },
    { L"test", L"test.png",
      L"Проверка: подсказка у часов + снимок экрана", TRUE, run_test },
};

const int g_scenario_count = (int)ARRAY_COUNT(g_scenarios);
