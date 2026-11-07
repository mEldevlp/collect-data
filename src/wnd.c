// Окна: запуск, поиск новых окон, ожидание готовности, раскладка, передний план, закрытие.
#include "wnd.h"
#include "buffer.h"
#include "capture.h"
#include "common.h"
#include "log.h"
#include <string.h>

// Права на чтение сведений о процессе: есть с Vista; на XP открываем с PROCESS_QUERY_INFORMATION
#ifndef PROCESS_QUERY_LIMITED_INFORMATION
#define PROCESS_QUERY_LIMITED_INFORMATION 0x1000
#endif

// SetWindowPos: поменять только положение в Z-порядке, не двигая окно и не активируя его
#define SWP_ZORDER_ONLY (SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE)

// Поиск окон
#define GET_TEXT_TIMEOUT_MS   500   // WM_GETTEXT чужому окну: не ответило - считаем, что заголовка нет
#define APP_WINDOW_MIN_WIDTH  150   // окно приложения не меньше 150x100 (меньше - значок, подсказка...)
#define APP_WINDOW_MIN_HEIGHT 100
#define TITLE_PROBE_CCH       8     // заголовок читаем только чтобы узнать, что он не пустой
#define DESCRIBE_TITLE_CCH    80    // заголовок в описании окна для журнала (длиннее - обрезается)
#define NEW_WINDOW_POLL_MS    150   // как часто искать новое окно

// Запомненные окна
#define MAX_TRACKED        32       // окон, открытых за прогон, - единицы; больше не запоминаем
#define TRACK_LOG_NAME_CCH 64

// Ленивое создание критической секции списка окон (первым может обратиться любой поток)
#define LOCK_NOT_INITIALIZED 0
#define LOCK_INITIALIZING    1
#define LOCK_READY           2
#define LOCK_SPIN_SLEEP_MS   1

// Ожидание готовности окна
#define READY_POLL_MS           300     // как часто снимать кадр окна
#define READY_GRAB_RETRY_MS     200     // кадр не снялся - подождать ещё
#define READY_NOISE_DIVISOR     2000    // изменение меньше 1/2000 площади окна (мигающая каретка) не в счёт,
#define READY_NOISE_MIN_PIXELS  64      // но порог не меньше 64 пикселей
#define READY_CPU_MAX_PERCENT   10      // процесс "почти без дела": меньше 10% процессора
#define READY_IO_MAX_OPERATIONS 40      // и меньше 40 операций ввода-вывода за время стабильности
#define FILETIME_UNITS_PER_MS   10000   // FILETIME считает интервалами по 100 нс

// Раскладка. Размеры - в пикселях при 96 DPI (масштаб 100%), на экране они умножаются на масштаб.
#define DEFAULT_DPI              96
#define RESTORE_WAIT_MS          300    // окно разворачивается из свёрнутого с анимацией
#define SYSINFO_WIDTH_PERCENT    45     // панель "Система" (Win7) - 45% ширины рабочей области,
#define SYSINFO_MIN_WIDTH        700    // но не уже 700: иначе имя компьютера обрезается ссылкой
                                        // "Изменить параметры" (проверено при 560)
#define SYSINFO_MAX_WIDTH        900
#define SYSINFO_BOTTOM_GAP       48     // полоса над часами: низ панели не уходит под подсказку с датой
#define SYSINFO_MIN_HEIGHT       420
#define TARGET_MIN_WIDTH_PERCENT 50     // целевое окно - не уже половины рабочей области
#define TARGET_MIN_WIDTH         520    // и не уже 520

// Закрытие
#define CLOSE_WAIT_MS           5000    // сколько ждать закрытия окна после WM_CLOSE
#define EMERGENCY_CLOSE_WAIT_MS 1500    // то же при аварийном завершении
#define CLOSE_POLL_MS           100
#define TERMINATE_WAIT_MS       2000    // ожидание выхода процесса после TerminateProcess
#define PROCESS_EXIT_WAIT_MS    3000    // наш процесс (rundll32 и т. п.) должен выйти сам после закрытия окна
#define CURSOR_RESTORE_DELAY_MS 200     // аварийно: дать основному потоку закончить движение курсора

// Поиск нового окна: условия и результат для new_window_enum_proc
typedef struct
{
    const WND_SET      *before;
    DWORD               process_id; // 0 - любой процесс
    const WCHAR *const *classes;    // NULL - любое окно приложения
    HWND                found;
} NEW_WINDOW_SEARCH;

// Что и как запускает wnd_open_ex
typedef struct
{
    const WCHAR        *command_line;
    const WCHAR        *work_dir;
    DWORD               create_flags;
    const STARTUPINFOW *startup_info;
    const WCHAR *const *classes;
    DWORD               timeout_ms;
    const WCHAR        *log_name;
} LAUNCH_REQUEST;

// Запомненное окно: закрыть в конце сценария или прогона
typedef struct
{
    HWND   window;
    DWORD  process_id;
    HANDLE process;             // наш процесс - можно завершить; иначе NULL
    WCHAR  log_name[TRACK_LOG_NAME_CCH];
} TRACKED_WINDOW;

// Активность процесса с момента его запуска
typedef struct
{
    ULONGLONG cpu_ms;           // процессорное время (ядро + пользователь)
    ULONGLONG io_operations;    // операций чтения и записи
} PROCESS_ACTIVITY;

// Состояние ожидания готовности окна
typedef struct
{
    CAP_FRAME        previous;          // последний кадр, на котором картинка изменилась
    CAP_FRAME        current;
    HANDLE           process;           // процесс окна; NULL - не открыть, смотрим только на картинку
    PROCESS_ACTIVITY activity_start;    // активность процесса в начале периода стабильности
    DWORD            start_tick;
    DWORD            stable_since;      // с какого момента картинка не меняется
    long             last_diff;         // сколько пикселей изменилось в последнем кадре
} READY_WAIT;

// Новый диалог-вопрос процесса (например, "Сохранить консоль?")
typedef struct
{
    WND_SET *seen;
    DWORD    process_id;
    HWND     found;
} QUESTION_SEARCH;

static TRACKED_WINDOW   g_tracked[MAX_TRACKED];
static int              g_tracked_count = 0;
static CRITICAL_SECTION g_tracked_lock;
static volatile LONG    g_tracked_lock_state = LOCK_NOT_INITIALIZED;
static volatile LONG    g_aborting = 0;

// Снимок списка окон

static BOOL CALLBACK snapshot_enum_proc(HWND window, LPARAM param)
{
    WND_SET *set = (WND_SET *)param;

    if (set->count >= WND_SET_MAX) return FALSE;
    if (IsWindowVisible(window)) set->list[set->count++] = window;

    return TRUE;
}

void wnd_snapshot(WND_SET *set)
{
    set->count = 0;
    EnumWindows(snapshot_enum_proc, (LPARAM)set);
}

BOOL wnd_in_set(const WND_SET *set, HWND window)
{
    for (int i = 0; i < set->count; i++)
    {
        if (set->list[i] == window) return TRUE;
    }

    return FALSE;
}

static void set_add(WND_SET *set, HWND window)
{
    if (set->count < WND_SET_MAX && !wnd_in_set(set, window)) set->list[set->count++] = window;
}

void wnd_title(HWND window, WCHAR *title, int title_cch)
{
    DWORD_PTR result = 0;

    title[0] = 0;

    if (!SendMessageTimeoutW(window, WM_GETTEXT, (WPARAM)title_cch, (LPARAM)title, SMTO_ABORTIFHUNG | SMTO_BLOCK,
                             GET_TEXT_TIMEOUT_MS, &result))
    {
        title[0] = 0;
    }

    title[title_cch - 1] = 0;
}

void wnd_describe(HWND window, WCHAR *description, int description_cch)
{
    WCHAR class_name[CLASS_NAME_CCH];
    WCHAR title[DESCRIBE_TITLE_CCH];
    DWORD process_id = 0;
    RECT rect = { 0, 0, 0, 0 };

    if (!GetClassNameW(window, class_name, CLASS_NAME_CCH)) class_name[0] = 0;

    wnd_title(window, title, DESCRIBE_TITLE_CCH);
    GetWindowThreadProcessId(window, &process_id);
    GetWindowRect(window, &rect);

    format_text(description, description_cch, L"%s \"%s\" PID %lu, %d,%d-%d,%d (%dx%d)", class_name, title,
                process_id, rect.left, rect.top, rect.right, rect.bottom, rect.right - rect.left,
                rect.bottom - rect.top);
}

// Поиск нового окна

// Окно приложения: видимое, с заголовком, не инструментальное, не меньше 150x100.
static BOOL is_app_window(HWND window)
{
    LONG ex_style = GetWindowLongW(window, GWL_EXSTYLE);
    LONG style = GetWindowLongW(window, GWL_STYLE);
    RECT rect;
    WCHAR title[TITLE_PROBE_CCH];

    if (ex_style & WS_EX_TOOLWINDOW) return FALSE;
    if ((style & WS_CAPTION) != WS_CAPTION) return FALSE;
    if (!GetWindowRect(window, &rect)) return FALSE;
    if (rect.right - rect.left < APP_WINDOW_MIN_WIDTH || rect.bottom - rect.top < APP_WINDOW_MIN_HEIGHT) return FALSE;

    wnd_title(window, title, TITLE_PROBE_CCH);

    return title[0] != 0;
}

// Класс окна - один из classes (список завершается NULL).
static BOOL class_in_list(HWND window, const WCHAR *const *classes)
{
    WCHAR class_name[CLASS_NAME_CCH];

    if (!GetClassNameW(window, class_name, CLASS_NAME_CCH)) return FALSE;

    for (const WCHAR *const *candidate = classes; *candidate; candidate++)
    {
        if (lstrcmpiW(class_name, *candidate) == 0) return TRUE;
    }

    return FALSE;
}

static BOOL CALLBACK new_window_enum_proc(HWND window, LPARAM param)
{
    NEW_WINDOW_SEARCH *search = (NEW_WINDOW_SEARCH *)param;
    DWORD process_id = 0;

    if (!IsWindowVisible(window) || wnd_in_set(search->before, window)) return TRUE;

    GetWindowThreadProcessId(window, &process_id);

    if (process_id == GetCurrentProcessId()) return TRUE;
    if (search->process_id && process_id != search->process_id) return TRUE;

    BOOL suitable = search->classes ? class_in_list(window, search->classes) : is_app_window(window);

    if (!suitable) return TRUE;

    search->found = window;

    return FALSE;
}

HWND wnd_wait_new(const WND_SET *before, DWORD process_id, const WCHAR *const *classes, DWORD timeout_ms)
{
    NEW_WINDOW_SEARCH search;
    DWORD start_tick = GetTickCount();

    search.before = before;
    search.process_id = process_id;
    search.classes = classes;

    for (;;)
    {
        search.found = NULL;
        EnumWindows(new_window_enum_proc, (LPARAM)&search);

        if (search.found) return search.found;
        if (wnd_aborting() || GetTickCount() - start_tick >= timeout_ms) return NULL;

        Sleep(NEW_WINDOW_POLL_MS);
    }
}

// В журнал - все окна, появившиеся с момента before (диагностика).
static BOOL CALLBACK log_new_enum_proc(HWND window, LPARAM param)
{
    const WND_SET *before = (const WND_SET *)param;
    WCHAR description[TEXT_BUFFER_CCH];

    if (IsWindowVisible(window) && !wnd_in_set(before, window))
    {
        wnd_describe(window, description, TEXT_BUFFER_CCH);
        log_msg(L"  новое окно: %s", description);
    }

    return TRUE;
}

// Запомненные окна

// Список окон трогают и основной поток, и аварийная уборка (сторожевой таймер, Ctrl+C) -
// критическая секция создаётся при первом обращении из любого из них.
static void tracked_lock_enter(void)
{
    LONG previous_state = InterlockedCompareExchange(&g_tracked_lock_state, LOCK_INITIALIZING, LOCK_NOT_INITIALIZED);

    if (previous_state == LOCK_NOT_INITIALIZED)
    {
        InitializeCriticalSection(&g_tracked_lock);
        InterlockedExchange(&g_tracked_lock_state, LOCK_READY);
    }

    while (g_tracked_lock_state != LOCK_READY) Sleep(LOCK_SPIN_SLEEP_MS);

    EnterCriticalSection(&g_tracked_lock);
}

static void tracked_lock_leave(void)
{
    LeaveCriticalSection(&g_tracked_lock);
}

void wnd_track(HWND window, HANDLE process, const WCHAR *log_name)
{
    tracked_lock_enter();

    if (g_tracked_count < MAX_TRACKED)
    {
        TRACKED_WINDOW *tracked = &g_tracked[g_tracked_count++];

        tracked->window = window;
        tracked->process_id = 0;
        GetWindowThreadProcessId(window, &tracked->process_id);
        tracked->process = process;
        lstrcpynW(tracked->log_name, log_name ? log_name : L"окно", TRACK_LOG_NAME_CCH);
    }
    else if (process)
    {
        CloseHandle(process);
    }

    tracked_lock_leave();
}

// Извлечь запись из списка (по окну; NULL - последнюю). FALSE - такой нет.
static BOOL track_take(HWND window, TRACKED_WINDOW *taken)
{
    BOOL found = FALSE;

    tracked_lock_enter();

    for (int i = g_tracked_count - 1; i >= 0; i--)
    {
        if (window && g_tracked[i].window != window) continue;

        *taken = g_tracked[i];
        memmove(&g_tracked[i], &g_tracked[i + 1], (size_t)(g_tracked_count - 1 - i) * sizeof(TRACKED_WINDOW));
        g_tracked_count--;
        found = TRUE;
        break;
    }

    tracked_lock_leave();

    return found;
}

// Запуск

HWND wnd_open(const WCHAR *command_line, const WCHAR *const *classes, DWORD timeout_ms, const WCHAR *log_name)
{
    return wnd_open_ex(command_line, NULL, 0, NULL, classes, timeout_ms, log_name, NULL);
}

// Изменяемая копия строки в buffer (CreateProcessW может менять переданную командную строку).
static BOOL copy_text(BUFFER *buffer, const WCHAR *text)
{
    if (buffer_init_array(buffer, wcslen(text) + 1, sizeof(WCHAR)) != BUFFER_OK) return FALSE;

    memcpy(buffer->data, text, buffer->size);

    return TRUE;
}

// Окно не появилось: записать в журнал все новые окна и завершить запущенный процесс.
static void log_open_timeout(const LAUNCH_REQUEST *request, const PROCESS_INFORMATION *process_info,
                             const WND_SET *before)
{
    log_msg(L"%s: окно не появилось за %lu мс", request->log_name, request->timeout_ms);
    EnumWindows(log_new_enum_proc, (LPARAM)before);

    if (WaitForSingleObject(process_info->hProcess, 0) == WAIT_TIMEOUT)
    {
        TerminateProcess(process_info->hProcess, TERMINATED_EXIT_CODE);
        log_msg(L"%s: процесс PID %lu завершён", request->log_name, process_info->dwProcessId);
    }
}

// Запустить command (изменяемая копия request->command_line) и дождаться нового окна; before -
// память под список окон до запуска. NULL - процесс не запустился (GetLastError - ошибка
// CreateProcess) или окно не появилось (ERROR_TIMEOUT).
static HWND launch_and_wait(const LAUNCH_REQUEST *request, WCHAR *command, WND_SET *before, DWORD *process_id_out)
{
    STARTUPINFOW startup_info;
    PROCESS_INFORMATION process_info;
    WCHAR description[TEXT_BUFFER_CCH];
    WCHAR error_text[TEXT_BUFFER_CCH];
    DWORD window_process_id = 0;

    wnd_snapshot(before);

    if (request->startup_info) startup_info = *request->startup_info;
    else ZeroMemory(&startup_info, sizeof(startup_info));

    startup_info.cb = sizeof(startup_info);
    startup_info.dwFlags |= STARTF_USESHOWWINDOW;
    startup_info.wShowWindow = SW_SHOWNORMAL;
    ZeroMemory(&process_info, sizeof(process_info));

    if (!CreateProcessW(NULL, command, NULL, NULL, FALSE, request->create_flags, NULL, request->work_dir,
                        &startup_info, &process_info))
    {
        DWORD error_code = GetLastError();

        log_msg(L"%s: не удалось запустить %s: %s", request->log_name, request->command_line,
                log_errtext(error_code, error_text, TEXT_BUFFER_CCH));
        SetLastError(error_code);       // вызывающий может проверить ERROR_ELEVATION_REQUIRED
        return NULL;
    }

    CloseHandle(process_info.hThread);
    log_msg(L"%s: запуск %s (PID %lu)", request->log_name, request->command_line, process_info.dwProcessId);

    DWORD start_tick = GetTickCount();
    HWND window = wnd_wait_new(before, 0, request->classes, request->timeout_ms);

    if (!window)
    {
        log_open_timeout(request, &process_info, before);
        CloseHandle(process_info.hProcess);
        SetLastError(ERROR_TIMEOUT);
        return NULL;
    }

    wnd_describe(window, description, TEXT_BUFFER_CCH);
    log_msg(L"%s: окно через %lu мс: %s", request->log_name, GetTickCount() - start_tick, description);

    if (process_id_out) *process_id_out = process_info.dwProcessId;

    GetWindowThreadProcessId(window, &window_process_id);

    if (window_process_id == process_info.dwProcessId)
    {
        wnd_track(window, process_info.hProcess, request->log_name);   // окно в нашем процессе - можно завершить
    }
    else
    {
        wnd_track(window, NULL, request->log_name);     // окно чужого процесса (explorer и т. п.)
        CloseHandle(process_info.hProcess);             // control.exe и т. п. завершится сам
    }

    return window;
}

HWND wnd_open_ex(const WCHAR *command_line, const WCHAR *work_dir, DWORD create_flags,
                 const STARTUPINFOW *startup_info_in, const WCHAR *const *classes, DWORD timeout_ms,
                 const WCHAR *log_name, DWORD *process_id_out)
{
    LAUNCH_REQUEST request;
    BUFFER before = { 0 };
    BUFFER command = { 0 };
    HWND window = NULL;
    DWORD last_error = ERROR_NOT_ENOUGH_MEMORY;

    if (process_id_out) *process_id_out = 0;
    if (wnd_aborting()) return NULL;

    request.command_line = command_line;
    request.work_dir = work_dir;
    request.create_flags = create_flags;
    request.startup_info = startup_info_in;
    request.classes = classes;
    request.timeout_ms = timeout_ms;
    request.log_name = log_name;

    // WND_SET большой (WND_SET_MAX окон) - не на стеке
    if (buffer_init(&before, sizeof(WND_SET)) == BUFFER_OK && copy_text(&command, command_line))
    {
        window = launch_and_wait(&request, command.data, before.data, process_id_out);
        last_error = GetLastError();
    }

    buffer_destroy(&command);
    buffer_destroy(&before);

    // код ошибки запуска нужен вызывающему, а освобождение памяти может его затереть
    if (!window) SetLastError(last_error);

    return window;
}

// Ожидание готовности

static HANDLE open_process_for_query(DWORD process_id)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);

    if (!process) process = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, process_id);

    return process;
}

static ULONGLONG filetime_units(const FILETIME *time)
{
    ULARGE_INTEGER value;

    value.LowPart = time->dwLowDateTime;
    value.HighPart = time->dwHighDateTime;

    return value.QuadPart;
}

static void process_activity(HANDLE process, PROCESS_ACTIVITY *activity)
{
    FILETIME creation_time, exit_time, kernel_time, user_time;
    IO_COUNTERS io_counters;

    activity->cpu_ms = 0;
    activity->io_operations = 0;

    if (!process) return;

    if (GetProcessTimes(process, &creation_time, &exit_time, &kernel_time, &user_time))
    {
        activity->cpu_ms = (filetime_units(&kernel_time) + filetime_units(&user_time)) / FILETIME_UNITS_PER_MS;
    }

    if (GetProcessIoCounters(process, &io_counters))
    {
        activity->io_operations = io_counters.ReadOperationCount + io_counters.WriteOperationCount;
    }
}

// Процесс почти ничего не делал за span_ms: мало процессорного времени и операций ввода-вывода.
static BOOL process_idle(const PROCESS_ACTIVITY *start, const PROCESS_ACTIVITY *now, DWORD span_ms)
{
    ULONGLONG cpu_ms = now->cpu_ms - start->cpu_ms;
    ULONGLONG io_operations = now->io_operations - start->io_operations;

    BOOL cpu_idle = cpu_ms * PERCENT_BASE < (ULONGLONG)span_ms * READY_CPU_MAX_PERCENT;

    return cpu_idle && io_operations < READY_IO_MAX_OPERATIONS;
}

void wnd_screen_rect(HWND window, RECT *rect)
{
    RECT screen;
    int left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int height = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    GetWindowRect(window, rect);
    SetRect(&screen, left, top, left + width, top + height);
    IntersectRect(rect, rect, &screen);
}

// Снять кадр окна и сравнить с предыдущим. TRUE - окно готово: картинка не менялась stable_ms,
// а процесс окна за это время почти ничего не делал.
static BOOL ready_check_frame(READY_WAIT *wait, HWND window, DWORD now, DWORD stable_ms)
{
    RECT rect;

    wnd_screen_rect(window, &rect);

    if (!cap_frame_grab(&rect, &wait->current))
    {
        Sleep(READY_GRAB_RETRY_MS);
        return FALSE;
    }

    // мелочь (мигающая каретка) не считается изменением
    long noise_limit = (long)wait->current.width * wait->current.height / READY_NOISE_DIVISOR;

    if (noise_limit < READY_NOISE_MIN_PIXELS) noise_limit = READY_NOISE_MIN_PIXELS;

    wait->last_diff = cap_frame_diff(&wait->previous, &wait->current);

    if (wait->last_diff == CAP_FRAME_DIFF_INVALID || wait->last_diff > noise_limit)
    {
        // картинка изменилась: новый кадр - образец, период стабильности начинается заново
        cap_frame_swap(&wait->previous, &wait->current);
        wait->stable_since = now;
        process_activity(wait->process, &wait->activity_start);
        return FALSE;
    }

    DWORD stable_span = now - wait->stable_since;

    if (stable_span < stable_ms) return FALSE;

    PROCESS_ACTIVITY activity_now;

    process_activity(wait->process, &activity_now);

    if (!wait->process || process_idle(&wait->activity_start, &activity_now, stable_span))
    {
        log_msg(L"Окно готово через %lu мс (картинка стабильна %lu мс, процесс: %I64u мс ЦП, %I64u оп. ввода-вывода)",
                now - wait->start_tick, stable_span, activity_now.cpu_ms - wait->activity_start.cpu_ms,
                activity_now.io_operations - wait->activity_start.io_operations);
        return TRUE;
    }

    // картинка стоит, но процесс занят - ждём дальше
    wait->stable_since = now;
    wait->activity_start = activity_now;

    return FALSE;
}

static void log_ready_timeout(const READY_WAIT *wait, DWORD timeout_ms)
{
    PROCESS_ACTIVITY activity_now;

    process_activity(wait->process, &activity_now);
    log_msg(L"Окно не успокоилось за %lu мс (последнее изменение: %ld пикс., процесс: %I64u мс ЦП, %I64u оп.) - "
            L"снимаю как есть", timeout_ms, wait->last_diff, activity_now.cpu_ms - wait->activity_start.cpu_ms,
            activity_now.io_operations - wait->activity_start.io_operations);
}

BOOL wnd_wait_ready(HWND window, DWORD stable_ms, DWORD timeout_ms)
{
    READY_WAIT wait;
    DWORD process_id = 0;
    BOOL ready = FALSE;

    ZeroMemory(&wait, sizeof(wait));
    wait.start_tick = GetTickCount();
    wait.last_diff = CAP_FRAME_DIFF_INVALID;

    GetWindowThreadProcessId(window, &process_id);
    wait.process = open_process_for_query(process_id);
    process_activity(wait.process, &wait.activity_start);
    wait.stable_since = GetTickCount();

    for (;;)
    {
        Sleep(READY_POLL_MS);

        DWORD now = GetTickCount();

        if (wnd_aborting()) break;

        if (!IsWindow(window))
        {
            log_msg(L"Ожидание готовности: окно закрылось");
            break;
        }

        if (ready_check_frame(&wait, window, now, stable_ms))
        {
            ready = TRUE;
            break;
        }

        if (now - wait.start_tick >= timeout_ms)
        {
            log_ready_timeout(&wait, timeout_ms);
            break;
        }
    }

    cap_frame_free(&wait.previous);
    cap_frame_free(&wait.current);

    if (wait.process) CloseHandle(wait.process);

    return ready;
}

// Раскладка

static int dpi_scale(int value)
{
    static int dpi = 0;

    if (!dpi)
    {
        HDC screen_dc = GetDC(NULL);

        dpi = screen_dc ? GetDeviceCaps(screen_dc, LOGPIXELSX) : DEFAULT_DPI;

        if (screen_dc) ReleaseDC(NULL, screen_dc);
        if (dpi <= 0) dpi = DEFAULT_DPI;
    }

    return MulDiv(value, dpi, DEFAULT_DPI);
}

static int clamp_int(int value, int low, int high)
{
    if (high < low) high = low;
    if (value < low) return low;
    if (value > high) return high;

    return value;
}

// Развернуть из свёрнутого или развёрнутого на весь экран состояния.
static void restore_window(HWND window)
{
    if (!IsIconic(window) && !IsZoomed(window)) return;

    ShowWindowAsync(window, SW_RESTORE);
    Sleep(RESTORE_WAIT_MS);
}

static BOOL movable(HWND window)
{
    return window && IsWindow(window) && !IsHungAppWindow(window);
}

static BOOL is_sizable(HWND window)
{
    return (GetWindowLongW(window, GWL_STYLE) & WS_THICKFRAME) != 0;
}

void wnd_work_area(RECT *area)
{
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, area, 0))
    {
        SetRect(area, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
    }
}

// Окно сведений о системе - справа. Возвращает его ширину (0 - окна нет).
// Диалог (XP "Свойства системы", "Имя компьютера") - своего размера, справа внизу, как в ручных
// примерах: подсказка у часов закрывает разве что кнопки OK/Отмена.
// Обычное окно (панель "Система" Win7) - ~45% ширины, справа сверху, по высоте - до полосы над
// часами: имя компьютера там в самом низу и иначе уходит под подсказку с датой (проверено на Win7).
static int place_sysinfo(HWND sysinfo, const RECT *area)
{
    RECT rect;
    int area_width = area->right - area->left;
    int area_height = area->bottom - area->top;

    if (!movable(sysinfo)) return 0;

    BOOL sizable = is_sizable(sysinfo);

    restore_window(sysinfo);
    GetWindowRect(sysinfo, &rect);

    int width = rect.right - rect.left;
    int height = rect.bottom - rect.top;

    if (sizable)
    {
        width = clamp_int(area_width * SYSINFO_WIDTH_PERCENT / PERCENT_BASE, dpi_scale(SYSINFO_MIN_WIDTH),
                          dpi_scale(SYSINFO_MAX_WIDTH));
        height = clamp_int(area_height - dpi_scale(SYSINFO_BOTTOM_GAP), dpi_scale(SYSINFO_MIN_HEIGHT), area_height);
    }

    if (width > area_width) width = area_width;
    if (height > area_height) height = area_height;

    int top = sizable ? area->top : area->bottom - height;

    SetWindowPos(sysinfo, NULL, area->right - width, top, width, height,
                 SWP_NOZORDER | SWP_NOACTIVATE | (sizable ? 0 : SWP_NOSIZE));
    GetWindowRect(sysinfo, &rect);
    log_msg(L"Раскладка: сведения о системе %d,%d %dx%d", rect.left, rect.top, rect.right - rect.left,
            rect.bottom - rect.top);

    return width;
}

void wnd_layout(HWND target, HWND sysinfo, RECT *target_rect)
{
    RECT area, rect;

    if (target_rect) SetRectEmpty(target_rect);

    wnd_work_area(&area);

    int area_width = area.right - area.left;
    int area_height = area.bottom - area.top;
    int sysinfo_width = place_sysinfo(sysinfo, &area);

    // Целевое окно - слева сверху, на всё оставшееся место (на маленьком экране - с перекрытием).
    // На 1024x768 рядом с диалогом XP (419 px) остаётся 605 - перекрытия быть не должно;
    // рядом с панелью "Система" Win7 (700) - 324: тогда 520, перекрыта только её левая
    // навигационная колонка (~200 px), сведения видны. Прямоугольник считается и без окна
    // (target = NULL): так узнают размер заранее, до запуска (консоль).
    int min_width = clamp_int(area_width * TARGET_MIN_WIDTH_PERCENT / PERCENT_BASE, dpi_scale(TARGET_MIN_WIDTH),
                              area_width);
    int width = area_width - sysinfo_width;

    if (width < min_width) width = min_width;
    if (target_rect) SetRect(target_rect, area.left, area.top, area.left + width, area.top + area_height);
    if (!movable(target)) return;

    BOOL sizable = is_sizable(target);

    restore_window(target);
    SetWindowPos(target, NULL, area.left, area.top, width, area_height,
                 SWP_NOZORDER | SWP_NOACTIVATE | (sizable ? 0 : SWP_NOSIZE));
    GetWindowRect(target, &rect);
    log_msg(L"Раскладка: целевое окно %d,%d %dx%d%s", rect.left, rect.top, rect.right - rect.left,
            rect.bottom - rect.top, sizable ? L"" : L" (размер не меняется)");
}

// Передний план

// Сделать окно активным: обход запрета SetForegroundWindow через AttachThreadInput.
static void bring_front(HWND window)
{
    HWND foreground = GetForegroundWindow();
    DWORD own_thread = GetCurrentThreadId();
    DWORD foreground_thread = foreground ? GetWindowThreadProcessId(foreground, NULL) : 0;
    BOOL attached = FALSE;

    if (foreground == window) return;

    AllowSetForegroundWindow(ASFW_ANY);

    if (foreground_thread && foreground_thread != own_thread)
    {
        attached = AttachThreadInput(own_thread, foreground_thread, TRUE);
    }

    BringWindowToTop(window);
    SetForegroundWindow(window);

    if (attached) AttachThreadInput(own_thread, foreground_thread, FALSE);
}

void wnd_raise(HWND *windows, int count)
{
    HWND last = NULL;

    for (int i = 0; i < count; i++)
    {
        if (!movable(windows[i])) continue;

        SetWindowPos(windows[i], HWND_TOPMOST, 0, 0, 0, 0, SWP_ZORDER_ONLY);
        last = windows[i];
    }

    if (!last) return;

    bring_front(last);

    if (GetForegroundWindow() != last)
    {
        log_msg(L"Внимание: не удалось сделать окно активным (оно поверх всех, но заголовок может быть неактивным)");
    }
}

void wnd_unraise(HWND *windows, int count)
{
    // HWND_NOTOPMOST ставит окно наверх обычных окон, поэтому - в прямом порядке: последнее
    // (целевое) остаётся сверху. В обратном порядке окно сведений вылезало поверх целевого и
    // на 800x600 закрывало часть списка (сбивало измерение прокрутки, проверено на XP).
    for (int i = 0; i < count; i++)
    {
        if (movable(windows[i])) SetWindowPos(windows[i], HWND_NOTOPMOST, 0, 0, 0, 0, SWP_ZORDER_ONLY);
    }
}

// Закрытие

static BOOL CALLBACK question_enum_proc(HWND window, LPARAM param)
{
    QUESTION_SEARCH *search = (QUESTION_SEARCH *)param;
    DWORD process_id = 0;
    WCHAR class_name[CLASS_NAME_CCH];

    if (!IsWindowVisible(window) || wnd_in_set(search->seen, window)) return TRUE;

    GetWindowThreadProcessId(window, &process_id);

    if (process_id != search->process_id) return TRUE;
    if (!GetClassNameW(window, class_name, CLASS_NAME_CCH) || lstrcmpW(class_name, WND_DIALOG_CLASS) != 0) return TRUE;

    search->found = window;

    return FALSE;
}

// Ответить на вопрос, появившийся при закрытии: "Нет", если есть такая кнопка, иначе закрыть его.
// seen - окна, которые уже видели (новый вопрос добавляется туда же).
static void answer_close_question(const TRACKED_WINDOW *tracked, WND_SET *seen)
{
    QUESTION_SEARCH search;
    WCHAR description[TEXT_BUFFER_CCH];

    search.seen = seen;
    search.process_id = tracked->process_id;
    search.found = NULL;
    EnumWindows(question_enum_proc, (LPARAM)&search);

    if (!search.found) return;

    HWND no_button = GetDlgItem(search.found, IDNO);

    wnd_describe(search.found, description, TEXT_BUFFER_CCH);
    set_add(seen, search.found);

    if (no_button)
    {
        log_msg(L"%s: вопрос при закрытии, отвечаю \"Нет\": %s", tracked->log_name, description);
        PostMessageW(search.found, WM_COMMAND, MAKEWPARAM(IDNO, BN_CLICKED), (LPARAM)no_button);
    }
    else
    {
        log_msg(L"%s: окно при закрытии, закрываю: %s", tracked->log_name, description);
        PostMessageW(search.found, WM_CLOSE, 0, 0);
    }
}

// Закрыть окно: WM_CLOSE и ожидание до wait_ms с ответами на вопросы; не закрылось - завершить
// процесс, если он наш.
static void close_window(const TRACKED_WINDOW *tracked, DWORD wait_ms)
{
    BUFFER seen_buffer = { 0 };
    WND_SET *seen = NULL;

    // окна до закрытия: всё новое после WM_CLOSE - вопросы ("Сохранить?"); WND_SET большой - не на стеке
    if (buffer_init(&seen_buffer, sizeof(WND_SET)) == BUFFER_OK)
    {
        seen = seen_buffer.data;
        wnd_snapshot(seen);
    }

    SetWindowPos(tracked->window, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_ZORDER_ONLY | SWP_ASYNCWINDOWPOS);
    PostMessageW(tracked->window, WM_CLOSE, 0, 0);

    DWORD start_tick = GetTickCount();

    while (IsWindow(tracked->window) && GetTickCount() - start_tick < wait_ms)
    {
        Sleep(CLOSE_POLL_MS);

        if (seen) answer_close_question(tracked, seen);
    }

    buffer_destroy(&seen_buffer);

    if (!IsWindow(tracked->window))
    {
        log_msg(L"%s: закрыто", tracked->log_name);
        return;
    }

    if (tracked->process)
    {
        TerminateProcess(tracked->process, TERMINATED_EXIT_CODE);
        WaitForSingleObject(tracked->process, TERMINATE_WAIT_MS);
        log_msg(L"%s: окно не закрылось за %lu мс, процесс PID %lu завершён", tracked->log_name, wait_ms,
                tracked->process_id);
    }
    else
    {
        log_msg(L"%s: окно не закрылось за %lu мс (процесс PID %lu не наш - не завершаю)", tracked->log_name, wait_ms,
                tracked->process_id);
    }
}

// Закрыть запомненное окно и отпустить его процесс.
static void close_tracked(const TRACKED_WINDOW *tracked, DWORD wait_ms)
{
    if (IsWindow(tracked->window)) close_window(tracked, wait_ms);
    if (!tracked->process) return;

    // наш процесс (rundll32 и т. п.) должен выйти сам после закрытия окна
    if (WaitForSingleObject(tracked->process, PROCESS_EXIT_WAIT_MS) == WAIT_TIMEOUT)
    {
        TerminateProcess(tracked->process, TERMINATED_EXIT_CODE);
        log_msg(L"%s: процесс PID %lu не завершился после закрытия окна - завершён", tracked->log_name,
                tracked->process_id);
    }

    CloseHandle(tracked->process);
}

void wnd_close(HWND window)
{
    TRACKED_WINDOW tracked;

    if (window && track_take(window, &tracked)) close_tracked(&tracked, CLOSE_WAIT_MS);
}

void wnd_close_all(BOOL keep)
{
    TRACKED_WINDOW tracked;

    while (track_take(NULL, &tracked))
    {
        if (!keep)
        {
            close_tracked(&tracked, CLOSE_WAIT_MS);
            continue;
        }

        if (IsWindow(tracked.window))
        {
            SetWindowPos(tracked.window, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_ZORDER_ONLY | SWP_ASYNCWINDOWPOS);
        }

        log_msg(L"%s: оставлено открытым (/keep)", tracked.log_name);

        if (tracked.process) CloseHandle(tracked.process);
    }
}

BOOL wnd_aborting(void)
{
    return g_aborting != 0;
}

void wnd_emergency_cleanup(void)
{
    TRACKED_WINDOW tracked;

    InterlockedExchange(&g_aborting, 1);

    while (track_take(NULL, &tracked)) close_tracked(&tracked, EMERGENCY_CLOSE_WAIT_MS);

    // курсор - последним: основной поток мог успеть навести его на часы (проверено)
    Sleep(CURSOR_RESTORE_DELAY_MS);
    wnd_clock_tip_hide();
}
