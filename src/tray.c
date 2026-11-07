// Панель задач: подсказка с датой у часов, "шарики" уведомлений.
#include "wnd.h"
#include "common.h"
#include "log.h"
#include <commctrl.h>   // только константа TTS_BALLOON - comctl32 не подключаем

#define TRAY_CLASS         L"Shell_TrayWnd"
#define TRAY_NOTIFY_CLASS  L"TrayNotifyWnd"
#define TRAY_CLOCK_CLASS   L"TrayClockWClass"
#define TOOLTIP_CLASS      L"tooltips_class32"
#define SHADOW_CLASS       L"SysShadow"     // тень окна на XP (CS_DROPSHADOW) - отдельное окно

// SendInput с MOUSEEVENTF_ABSOLUTE: координаты основного монитора - от 0 до 65535 по каждой оси
#define ABSOLUTE_COORD_MAX 65535

#define GLIDE_STEP_MS 25    // пауза между шагами подхода курсора к часам

// Win11 и др.: часы - не отдельное окно; берём полосу у правого (нижнего) края панели задач
#define CLOCK_GUESS_WIDTH      90   // горизонтальная панель: 90 пикселей у правого края,
#define CLOCK_GUESS_RIGHT_GAP  10   // кроме последних 10 ("Показать рабочий стол")
#define CLOCK_GUESS_HEIGHT     60   // вертикальная панель: 60 пикселей у нижнего края

#define TIP_MAX_DISTANCE       400  // подсказка часов - не дальше 400 пикселей от них
#define TIP_POLL_MS            100
#define TIP_REHOVER_PERIOD_MS  1000 // раз в секунду увести курсор и навести заново
#define TIP_REHOVER_LIFT       60   // на сколько пикселей уводить курсор вверх
#define TIP_REHOVER_PAUSE_MS   50

#define BALLOON_REDRAW_MARGIN  12   // тень шарика выходит за его края - перерисовать с запасом
#define BALLOON_HIDE_SETTLE_MS 300  // дать Explorer перерисовать место под шариками

// Подход курсора к точке: смещения последовательных шагов (слева сверху - как живая мышь)
static const int GLIDE_OFFSET_X[] = { -30, -15, -6, -2, 0 };
static const int GLIDE_OFFSET_Y[] = { -12, -6, -3, -1, 0 };

// Поиск подсказки у часов
typedef struct
{
    DWORD          process_id;      // процесс панели задач
    const WND_SET *before;
    RECT           clock_rect;
    HWND           found;
    WCHAR          class_name[CLASS_NAME_CCH];
    BOOL           log_new;         // режим диагностики: записать все новые окна
} TIP_SEARCH;

// Спрятанные "шарики" уведомлений
typedef struct
{
    DWORD process_id;
    int   hidden;
    RECT  area;                     // объединение прямоугольников спрятанных шариков
} BALLOON_SEARCH;

static volatile LONG g_cursor_saved = 0;
static POINT         g_cursor_position;

// Движение мыши

// Настоящее перемещение через SendInput: панель задач Win8+ реагирует на pointer-ввод,
// а SetCursorPos его не порождает.
static void mouse_move_to(int x, int y)
{
    INPUT input;
    int screen_width = GetSystemMetrics(SM_CXSCREEN);
    int screen_height = GetSystemMetrics(SM_CYSCREEN);

    if (screen_width <= 1 || screen_height <= 1)
    {
        SetCursorPos(x, y);
        return;
    }

    // пиксели -> 0..65535 с округлением до ближайшего
    ZeroMemory(&input, sizeof(input));
    input.type = INPUT_MOUSE;
    input.mi.dx = (LONG)(((LONGLONG)x * ABSOLUTE_COORD_MAX + (screen_width - 1) / 2) / (screen_width - 1));
    input.mi.dy = (LONG)(((LONGLONG)y * ABSOLUTE_COORD_MAX + (screen_height - 1) / 2) / (screen_height - 1));
    input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;

    if (SendInput(1, &input, sizeof(INPUT)) != 1) SetCursorPos(x, y);    // SendInput заблокирован (UIPI и т. п.)
}

// Подвести курсор к точке несколькими шагами - как живая мышь.
static void mouse_glide_to(int x, int y)
{
    for (size_t i = 0; i < ARRAY_COUNT(GLIDE_OFFSET_X); i++)
    {
        mouse_move_to(x + GLIDE_OFFSET_X[i], y + GLIDE_OFFSET_Y[i]);
        Sleep(GLIDE_STEP_MS);
    }
}

// Часы

// Прямоугольник часов. FALSE - часов нет; тогда rect - примерное место (край панели задач).
static BOOL find_clock(HWND *tray_out, RECT *rect)
{
    HWND tray = FindWindowW(TRAY_CLASS, NULL);
    HWND clock = NULL;

    *tray_out = tray;

    if (!tray) return FALSE;

    HWND notify_area = FindWindowExW(tray, NULL, TRAY_NOTIFY_CLASS, NULL);

    if (notify_area) clock = FindWindowExW(notify_area, NULL, TRAY_CLOCK_CLASS, NULL);

    if (clock && IsWindowVisible(clock) && GetWindowRect(clock, rect) && rect->right > rect->left &&
        rect->bottom > rect->top)
    {
        return TRUE;
    }

    // Win11 и др.: часы не отдельное окно. Берём правый (или нижний) край панели задач.
    if (!GetWindowRect(tray, rect)) return FALSE;

    int tray_width = rect->right - rect->left;
    int tray_height = rect->bottom - rect->top;

    if (tray_width >= tray_height)
    {
        // горизонтальная панель
        rect->left = rect->right - CLOCK_GUESS_WIDTH;

        if (rect->left < 0) rect->left = 0;

        rect->right -= CLOCK_GUESS_RIGHT_GAP;
    }
    else
    {
        // вертикальная панель
        rect->top = rect->bottom - CLOCK_GUESS_HEIGHT;
    }

    return FALSE;
}

// Окно в пределах TIP_MAX_DISTANCE пикселей от часов.
static BOOL near_clock(const RECT *rect, const RECT *clock_rect)
{
    return !(rect->right < clock_rect->left - TIP_MAX_DISTANCE || rect->left > clock_rect->right + TIP_MAX_DISTANCE ||
             rect->bottom < clock_rect->top - TIP_MAX_DISTANCE || rect->top > clock_rect->bottom + TIP_MAX_DISTANCE);
}

static BOOL CALLBACK tip_enum_proc(HWND window, LPARAM param)
{
    TIP_SEARCH *search = (TIP_SEARCH *)param;
    DWORD process_id = 0;
    WCHAR class_name[CLASS_NAME_CCH];
    RECT rect;

    if (!IsWindowVisible(window)) return TRUE;

    GetWindowThreadProcessId(window, &process_id);

    if (!GetClassNameW(window, class_name, CLASS_NAME_CCH)) class_name[0] = 0;
    if (!GetWindowRect(window, &rect)) return TRUE;

    BOOL is_new = !wnd_in_set(search->before, window);

    if (search->log_new)
    {
        if (is_new)
        {
            log_msg(L"  новое окно: класс %s, PID %lu, %d,%d-%d,%d", class_name, process_id, rect.left, rect.top,
                    rect.right, rect.bottom);
        }

        return TRUE;
    }

    // Только окно, ставшее видимым после наведения. "Шарики" уведомлений (TTS_BALLOON)
    // - не подсказка часов, хотя это тоже tooltips_class32 у трея.
    if (!near_clock(&rect, &search->clock_rect) || !is_new) return TRUE;
    if (GetWindowLongW(window, GWL_STYLE) & TTS_BALLOON) return TRUE;

    if (lstrcmpiW(class_name, TOOLTIP_CLASS) == 0 || process_id == search->process_id)
    {
        search->found = window;
        lstrcpynW(search->class_name, class_name, CLASS_NAME_CCH);
        return FALSE;
    }

    return TRUE;
}

// Спрятать видимые "шарики" уведомлений панели задач (XP: "Параметры экрана", "Безопасность
// компьютера"...): они закрывают часы и окна. Только ShowWindow(SW_HIDE): TTM_POP заставляет
// Explorer сразу показать следующий шарик из очереди (проверено на XP SP2).
static BOOL CALLBACK balloon_enum_proc(HWND window, LPARAM param)
{
    BALLOON_SEARCH *search = (BALLOON_SEARCH *)param;
    DWORD process_id = 0;
    WCHAR class_name[CLASS_NAME_CCH];
    RECT rect;

    if (!IsWindowVisible(window)) return TRUE;

    GetWindowThreadProcessId(window, &process_id);

    if (process_id != search->process_id) return TRUE;
    if (!GetClassNameW(window, class_name, CLASS_NAME_CCH) || lstrcmpiW(class_name, TOOLTIP_CLASS) != 0) return TRUE;
    if (!(GetWindowLongW(window, GWL_STYLE) & TTS_BALLOON)) return TRUE;
    if (GetWindowRect(window, &rect)) UnionRect(&search->area, &search->area, &rect);

    ShowWindow(window, SW_HIDE);
    search->hidden++;

    return TRUE;
}

// Тень окна на XP (CS_DROPSHADOW) - отдельное окно SysShadow; при SW_HIDE чужого шарика
// она остаётся на экране тёмной полосой (проверено на XP SP2). Прячем тени в области шариков.
static BOOL CALLBACK shadow_enum_proc(HWND window, LPARAM param)
{
    BALLOON_SEARCH *search = (BALLOON_SEARCH *)param;
    WCHAR class_name[CLASS_NAME_CCH];
    RECT rect, overlap;

    if (!IsWindowVisible(window)) return TRUE;
    if (!GetClassNameW(window, class_name, CLASS_NAME_CCH) || lstrcmpiW(class_name, SHADOW_CLASS) != 0) return TRUE;
    if (GetWindowRect(window, &rect) && IntersectRect(&overlap, &rect, &search->area)) ShowWindow(window, SW_HIDE);

    return TRUE;
}

int wnd_hide_tray_balloons(void)
{
    BALLOON_SEARCH search;
    HWND tray = FindWindowW(TRAY_CLASS, NULL);

    ZeroMemory(&search, sizeof(search));

    if (!tray) return 0;

    GetWindowThreadProcessId(tray, &search.process_id);

    if (!search.process_id) return 0;

    EnumWindows(balloon_enum_proc, (LPARAM)&search);

    if (!search.hidden) return 0;

    // XP не всегда перерисовывает место под шариком и его тенью - заставляем
    InflateRect(&search.area, BALLOON_REDRAW_MARGIN, BALLOON_REDRAW_MARGIN);
    EnumWindows(shadow_enum_proc, (LPARAM)&search);
    RedrawWindow(NULL, &search.area, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
    log_msg(L"Спрятано всплывающих уведомлений у трея: %d (%d,%d-%d,%d)", search.hidden, search.area.left,
            search.area.top, search.area.right, search.area.bottom);
    Sleep(BALLOON_HIDE_SETTLE_MS);

    return search.hidden;
}

// Ждать подсказку до timeout_ms, раз в секунду наводя курсор заново (если первое наведение
// не сработало). TRUE - подсказка появилась.
static BOOL wait_clock_tip(TIP_SEARCH *search, int center_x, int center_y, DWORD timeout_ms)
{
    DWORD start_tick = GetTickCount();
    DWORD next_rehover = TIP_REHOVER_PERIOD_MS;

    for (;;)
    {
        search->found = NULL;
        EnumWindows(tip_enum_proc, (LPARAM)search);

        if (search->found)
        {
            RECT rect;

            GetWindowRect(search->found, &rect);
            log_msg(L"Часы: подсказка появилась через %lu мс (класс %s, %d,%d-%d,%d)", GetTickCount() - start_tick,
                    search->class_name, rect.left, rect.top, rect.right, rect.bottom);
            return TRUE;
        }

        DWORD elapsed = GetTickCount() - start_tick;

        if (elapsed >= timeout_ms || wnd_aborting()) return FALSE;

        Sleep(TIP_POLL_MS);

        if (elapsed >= next_rehover)
        {
            next_rehover += TIP_REHOVER_PERIOD_MS;
            mouse_move_to(center_x, center_y - TIP_REHOVER_LIFT);
            Sleep(TIP_REHOVER_PAUSE_MS);
            mouse_glide_to(center_x, center_y);
        }
    }
}

BOOL wnd_clock_tip_show(DWORD timeout_ms)
{
    static WND_SET before;      // большой - не на стеке
    HWND tray;
    RECT clock_rect;
    TIP_SEARCH search;

    if (wnd_aborting()) return FALSE;

    BOOL is_clock = find_clock(&tray, &clock_rect);

    if (!tray)
    {
        log_msg(L"Часы: нет панели задач (Shell_TrayWnd), дата у часов не будет показана");
        return FALSE;
    }

    if (!is_clock) log_msg(L"Часы: окно TrayClockWClass не найдено, навожу курсор на правый край панели задач");

    wnd_hide_tray_balloons();
    ZeroMemory(&search, sizeof(search));
    GetWindowThreadProcessId(tray, &search.process_id);
    wnd_snapshot(&before);

    // запомнить курсор один раз - вернуть его на место в wnd_clock_tip_hide
    if (!InterlockedExchange(&g_cursor_saved, 1))
    {
        if (!GetCursorPos(&g_cursor_position))
        {
            g_cursor_position.x = 0;
            g_cursor_position.y = 0;
        }
    }

    int center_x = (clock_rect.left + clock_rect.right) / 2;
    int center_y = (clock_rect.top + clock_rect.bottom) / 2;

    log_msg(L"Часы: %s %d,%d-%d,%d, навожу курсор в %d,%d", is_clock ? TRAY_CLOCK_CLASS : L"примерно",
            clock_rect.left, clock_rect.top, clock_rect.right, clock_rect.bottom, center_x, center_y);
    mouse_glide_to(center_x, center_y);

    search.before = &before;
    search.clock_rect = clock_rect;

    if (wait_clock_tip(&search, center_x, center_y, timeout_ms)) return TRUE;

    POINT cursor;

    GetCursorPos(&cursor);
    log_msg(L"Часы: подсказка не появилась за %lu мс (курсор в %d,%d), снимаю без неё. Новые окна:", timeout_ms,
            cursor.x, cursor.y);
    search.log_new = TRUE;
    EnumWindows(tip_enum_proc, (LPARAM)&search);

    return FALSE;
}

void wnd_clock_tip_hide(void)
{
    if (InterlockedExchange(&g_cursor_saved, 0)) SetCursorPos(g_cursor_position.x, g_cursor_position.y);
}
