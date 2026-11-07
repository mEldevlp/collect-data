// Постраничная съёмка списка: прокрутка колесом мыши, сдвиг измеряется сравнением кадров.
//
// Идея. Пусть H - полная высота списка, V - видимая высота. Тогда длина прокрутки S = H - V,
// страниц n = 1 + ceil(S / (V - перекрытие)), шаг = S / (n - 1): все страницы сдвинуты на
// одинаковый шаг, перекрытие соседних одинаковое, последняя - ровно конец списка.
// S и V заранее неизвестны (у DirectUI-списков нет обычной полосы прокрутки), поэтому:
// 1) колесо на 1 щелчок: изменившаяся область кадра = видимая часть списка (V), а сдвиг
//    картинки (сравнение строк пикселей) = p пикселей на щелчок;
// 2) крутим до конца (картинка перестала меняться), суммируя измеренные сдвиги = S;
// 3) возвращаемся наверх и снимаем страницы, прокручивая на шаг (с точностью до щелчка).
// В коде V - visible_px, S - scroll_length, p - notch_px.

#include "scroll.h"
#include "buffer.h"
#include "capture.h"
#include "common.h"
#include "log.h"
#include "wnd.h"

#define MAX_NOTCHES      3000   // защита от бесконечной прокрутки
#define MAX_DEBUG_FRAMES 6      // пар диагностических кадров за прогон - не больше

// scroll_and_measure: сдвиг не измерить
#define SHIFT_UNKNOWN (-1)

// Ожидание стабильной картинки после прокрутки (плавная прокрутка, перерисовка)
#define STABLE_GRAB_INTERVAL_MS 120
#define STABLE_GRAB_TIMEOUT_MS  1500

// XP ARP при частых щелчках колеса часть теряет (12 щелчков по 25 мс дали 140 px вместо 360)
#define WHEEL_NOTCH_INTERVAL_MS 60

// FNV-1a, 32 бита: хэш строки пикселей
#define FNV_OFFSET_BASIS 2166136261u
#define FNV_PRIME        16777619u

// Поиск сдвига между кадрами
#define SHIFT_MIN_OVERLAP_DIVISOR 8     // перекрытие кадров - не меньше 1/8 видимой части,
#define SHIFT_MIN_OVERLAP_ROWS    20    // но не меньше 20 проверенных строк
#define SHIFT_MAX_MARGIN_ROWS     4     // сдвиг меньше видимой высоты хотя бы на 4 строки
#define SHIFT_MIN_MATCH_PERCENT   90    // надёжно: совпали не меньше 90% проверенных строк

// Куда ставить курсор для колеса: правее середины окна (слева у всех этих окон панель
// задач/ссылок), чуть ниже середины по высоте
#define WHEEL_POINT_X_PERCENT 62
#define WHEEL_POINT_Y_PERCENT 55

// Различий не больше 1/50 (2%) площади окна - "картинка не изменилась" (подсказка и т. п.)
#define STILL_AREA_DIVISOR 50

// Полосу прокрутки (бегунок двигается) не сравниваем: её ширина + 4 пикселя рамки.
// Если без неё полоса сравнения уже 50 пикселей - полосу прокрутки не вычитаем.
#define SCROLLBAR_MARGIN       4
#define COMPARE_BAND_MIN_WIDTH 50

#define STEP_VISIBLE_PERCENT  60    // за один шаг крутим ~60% видимой части
#define REWIND_EXTRA_NOTCHES  5     // наверх - с запасом щелчков
#define TOP_CHECK_MAX_STEPS   20    // сколько раз докручивать вверх, если до начала не дошли
#define OVERLAP_NOTCH_DIVISOR 3     // перекрытие страниц - не меньше трети щелчка (~ строка),
#define MIN_OVERLAP_PX        8     // но не меньше 8 пикселей
#define MAX_APPROACH_STEPS    40    // шагов прокрутки на подход к странице
#define MAX_TAIL_NOTCHES      20    // последняя страница: докрутить до конца, не больше 20 щелчков
#define UNDO_EXTRA_NOTCHES    2     // измерение не удалось - вернуть список в начало с запасом
#define UNDO_SETTLE_MS        300

// Состояние постраничной съёмки
typedef struct
{
    RECT      window_rect;          // окно на экране (область кадров)
    POINT     wheel_point;          // куда ставить курсор для колеса
    int       band_left;            // полоса для сравнения строк (без полосы прокрутки), x в кадре
    int       band_right;
    int       list_top;             // видимая часть списка, y в кадре
    int       list_bottom;
    long      still_limit;          // различий не больше - "картинка не изменилась"
    BUFFER    hashes_before;        // DWORD на строку кадра before - хэш строки
    BUFFER    hashes_after;
    BUFFER    uniform_before;       // BYTE на строку кадра before - строка одного цвета
    BUFFER    uniform_after;
    CAP_FRAME before;               // кадр до прокрутки (после scroll_and_measure - текущий)
    CAP_FRAME after;
    CAP_FRAME scratch;              // рабочий кадр для grab_stable
    CAP_FRAME top;                  // кадр самого начала списка
    int       visible_px;           // V - высота видимой части списка
    int       notch_px;             // p - сдвиг на один щелчок колеса
    int       notches_per_step;     // щелчков за один шаг (~60% видимой части)
    int       total_notches;        // сколько щелчков вниз сделано при измерении
} SCROLL_STATE;

// Раскладка на страницы
typedef struct
{
    int  page_count;
    int  step_px;                   // шаг между соседними страницами
    BOOL truncated;                 // список длиннее max_pages страниц - сняты первые подряд
} PAGE_PLAN;

static const WCHAR *g_debug_dir = NULL;
static int          g_debug_frame_count = 0;

void scroll_set_debug_dir(const WCHAR *dir)
{
    g_debug_dir = dir;
}

static void save_debug_frames(const CAP_FRAME *before, const CAP_FRAME *after, int score)
{
    WCHAR path[PATH_BUFFER_CCH];

    if (!g_debug_dir || g_debug_frame_count >= MAX_DEBUG_FRAMES) return;

    g_debug_frame_count++;

    format_text(path, PATH_BUFFER_CCH, L"%s\\_прокрутка_%d_до.png", g_debug_dir, g_debug_frame_count);
    cap_frame_png(before, path);

    format_text(path, PATH_BUFFER_CCH, L"%s\\_прокрутка_%d_после.png", g_debug_dir, g_debug_frame_count);
    cap_frame_png(after, path);

    log_msg(L"Прокрутка: кадры неудачного сравнения (совпало %d%% строк) - _прокрутка_%d_*.png", score,
            g_debug_frame_count);
}

// Кадры

// Снять кадр, когда картинка перестала меняться (плавная прокрутка, перерисовка), до 1,5 с.
static BOOL grab_stable(const RECT *rect, CAP_FRAME *frame, CAP_FRAME *scratch)
{
    DWORD start_tick = GetTickCount();

    Sleep(STABLE_GRAB_INTERVAL_MS);

    if (!cap_frame_grab(rect, frame)) return FALSE;

    for (;;)
    {
        Sleep(STABLE_GRAB_INTERVAL_MS);

        if (!cap_frame_grab(rect, scratch)) return FALSE;

        long different = cap_frame_diff(frame, scratch);

        cap_frame_swap(frame, scratch);

        if (different == 0 || GetTickCount() - start_tick > STABLE_GRAB_TIMEOUT_MS) return TRUE;
    }
}

// Прямоугольник различий двух кадров (в координатах кадра) и число различных пикселей
// (CAP_FRAME_DIFF_INVALID - кадры нельзя сравнить).
static long diff_bounds(const CAP_FRAME *first, const CAP_FRAME *second, RECT *bounds)
{
    long different = 0;

    // пустой прямоугольник "наизнанку": первое же различие сожмёт его до одной точки
    SetRect(bounds, first->width, first->height, -1, -1);

    if (!first->pixels.data || !second->pixels.data) return CAP_FRAME_DIFF_INVALID;
    if (first->width != second->width || first->height != second->height) return CAP_FRAME_DIFF_INVALID;

    const DWORD *first_pixels = first->pixels.data;
    const DWORD *second_pixels = second->pixels.data;

    for (int y = 0; y < first->height; y++)
    {
        const DWORD *first_row = first_pixels + (size_t)y * first->width;
        const DWORD *second_row = second_pixels + (size_t)y * second->width;

        for (int x = 0; x < first->width; x++)
        {
            if (((first_row[x] ^ second_row[x]) & PIXEL_RGB_MASK) == 0) continue;

            different++;

            if (x < bounds->left) bounds->left = x;
            if (x > bounds->right) bounds->right = x;
            if (y < bounds->top) bounds->top = y;
            if (y > bounds->bottom) bounds->bottom = y;
        }
    }

    // правая и нижняя стороны RECT - не включительно
    if (different)
    {
        bounds->right++;
        bounds->bottom++;
    }

    return different;
}

// Хэши строк кадра в полосе [band_left, band_right) и признак "строка одного цвета" (пустые
// строки совпадают с чем угодно - при сравнении их не учитываем).
static void row_hashes(const CAP_FRAME *frame, int band_left, int band_right, DWORD *hashes, BYTE *uniform)
{
    const DWORD *pixels = frame->pixels.data;

    for (int y = 0; y < frame->height; y++)
    {
        const DWORD *row = pixels + (size_t)y * frame->width;
        DWORD hash = FNV_OFFSET_BASIS;
        DWORD first_color = row[band_left] & PIXEL_RGB_MASK;
        BYTE same_color = TRUE;

        for (int x = band_left; x < band_right; x++)
        {
            DWORD color = row[x] & PIXEL_RGB_MASK;

            hash = (hash ^ color) * FNV_PRIME;

            if (color != first_color) same_color = FALSE;
        }

        hashes[y] = hash;
        uniform[y] = same_color;
    }
}

// Насколько содержимое уехало вверх между кадрами first и second (second[y] == first[y + shift])
// в строках [top, bottom). Возвращает сдвиг или 0, если надёжно не совпало; *score - процент
// совпавших строк у лучшего сдвига.
// Выбор - по ДОЛЕ совпавших строк, а не по их числу: в списке из одинаковых строк
// ("Screens Test Program 01", "...02") строки пикселей над и под текстом одинаковы у всех
// пунктов, и сдвиг на кратное высоте пункта тоже "совпадает" на ~70%; по числу совпадений
// выигрывал меньший сдвиг (больше перекрытие) - длина прокрутки занижалась втрое (проверено
// на XP 800x600). Истинный сдвиг даёт ~100% (кроме строк под подсказкой/подсветкой).
static int find_shift(const DWORD *first_hashes, const DWORD *second_hashes, const BYTE *second_uniform, int top,
                      int bottom, int *score)
{
    int best_shift = 0;
    int best_matched = 0;
    int best_checked = 0;
    int min_checked = (bottom - top) / SHIFT_MIN_OVERLAP_DIVISOR;

    if (min_checked < SHIFT_MIN_OVERLAP_ROWS) min_checked = SHIFT_MIN_OVERLAP_ROWS;

    for (int shift = 1; shift < bottom - top - SHIFT_MAX_MARGIN_ROWS; shift++)
    {
        int matched = 0;
        int checked = 0;

        for (int y = top; y + shift < bottom; y++)
        {
            if (second_uniform[y]) continue;

            checked++;

            if (second_hashes[y] == first_hashes[y + shift]) matched++;
        }

        if (checked < min_checked) continue;

        // доля matched / checked больше лучшей; при равной доле - больше совпавших строк
        LONGLONG share = (LONGLONG)matched * best_checked;
        LONGLONG best_share = (LONGLONG)best_matched * checked;

        if (best_checked == 0 || share > best_share || (share == best_share && matched > best_matched))
        {
            best_matched = matched;
            best_shift = shift;
            best_checked = checked;
        }
    }

    *score = best_checked ? best_matched * PERCENT_BASE / best_checked : 0;

    if (best_checked == 0 || best_matched * PERCENT_BASE < best_checked * SHIFT_MIN_MATCH_PERCENT) return 0;

    return best_shift;
}

// Сдвиг между кадрами before и after по строкам видимой части списка; notches > 0 - прокрутка
// вниз (содержимое уехало вверх), иначе вверх.
static int measure_shift(SCROLL_STATE *state, int notches, int *score)
{
    DWORD *hashes_before = state->hashes_before.data;
    DWORD *hashes_after = state->hashes_after.data;
    BYTE *uniform_before = state->uniform_before.data;
    BYTE *uniform_after = state->uniform_after.data;

    row_hashes(&state->before, state->band_left, state->band_right, hashes_before, uniform_before);
    row_hashes(&state->after, state->band_left, state->band_right, hashes_after, uniform_after);

    if (notches > 0)
    {
        return find_shift(hashes_before, hashes_after, uniform_after, state->list_top, state->list_bottom, score);
    }

    return find_shift(hashes_after, hashes_before, uniform_before, state->list_top, state->list_bottom, score);
}

// Колесо

// Прокрутить колесом на notches щелчков (> 0 - вниз).
static void wheel_scroll(int notches)
{
    INPUT input;
    int count = notches > 0 ? notches : -notches;

    for (int i = 0; i < count && !wnd_aborting(); i++)
    {
        ZeroMemory(&input, sizeof(input));
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = MOUSEEVENTF_WHEEL;
        input.mi.mouseData = (DWORD)(notches > 0 ? -WHEEL_DELTA : WHEEL_DELTA);
        SendInput(1, &input, sizeof(INPUT));
        Sleep(WHEEL_NOTCH_INTERVAL_MS);
    }
}

// Прокрутить на notches и измерить сдвиг. before - кадр до (на выходе - кадр после).
// Возвращает сдвиг в пикселях (0 - не сдвинулось), SHIFT_UNKNOWN - сдвиг не измерить.
static int scroll_and_measure(SCROLL_STATE *state, int notches)
{
    RECT bounds;
    int score;

    SetCursorPos(state->wheel_point.x, state->wheel_point.y);
    wheel_scroll(notches);

    if (!grab_stable(&state->window_rect, &state->after, &state->scratch)) return SHIFT_UNKNOWN;

    long different = diff_bounds(&state->before, &state->after, &bounds);

    if (different != CAP_FRAME_DIFF_INVALID && different <= state->still_limit)
    {
        cap_frame_swap(&state->before, &state->after);
        return 0;
    }

    int shift = measure_shift(state, notches, &score);

    if (shift <= 0) save_debug_frames(&state->before, &state->after, score);

    cap_frame_swap(&state->before, &state->after);

    return shift > 0 ? shift : SHIFT_UNKNOWN;
}

// Шаги съёмки

static void scroll_state_init(SCROLL_STATE *state, HWND window)
{
    ZeroMemory(state, sizeof(*state));
    wnd_screen_rect(window, &state->window_rect);

    int width = state->window_rect.right - state->window_rect.left;
    int height = state->window_rect.bottom - state->window_rect.top;

    state->wheel_point.x = state->window_rect.left + width * WHEEL_POINT_X_PERCENT / PERCENT_BASE;
    state->wheel_point.y = state->window_rect.top + height * WHEEL_POINT_Y_PERCENT / PERCENT_BASE;
    state->still_limit = (long)width * height / STILL_AREA_DIVISOR;
}

static void scroll_state_free(SCROLL_STATE *state)
{
    buffer_destroy(&state->hashes_before);
    buffer_destroy(&state->hashes_after);
    buffer_destroy(&state->uniform_before);
    buffer_destroy(&state->uniform_after);

    cap_frame_free(&state->before);
    cap_frame_free(&state->after);
    cap_frame_free(&state->scratch);
    cap_frame_free(&state->top);
}

static BOOL alloc_row_buffers(SCROLL_STATE *state, int rows)
{
    return buffer_init_array(&state->hashes_before, (size_t)rows, sizeof(DWORD)) == BUFFER_OK &&
           buffer_init_array(&state->hashes_after, (size_t)rows, sizeof(DWORD)) == BUFFER_OK &&
           buffer_init(&state->uniform_before, (size_t)rows) == BUFFER_OK &&
           buffer_init(&state->uniform_after, (size_t)rows) == BUFFER_OK;
}

// Вернуть список в начало после неудачного измерения.
static void undo_scroll(const SCROLL_STATE *state)
{
    wheel_scroll(-(state->total_notches + UNDO_EXTRA_NOTCHES));
    Sleep(UNDO_SETTLE_MS);
}

// Шаг 1: один щелчок колеса - видимая часть списка и сдвиг на щелчок. FALSE - прокрутки нет или
// сдвиг не измерить (тогда список уже возвращён в начало) - снимать одну страницу.
static BOOL measure_first_notch(SCROLL_STATE *state, BOOL dump)
{
    RECT bounds;
    int score;

    SetCursorPos(state->wheel_point.x, state->wheel_point.y);

    if (!grab_stable(&state->window_rect, &state->before, &state->scratch)) return FALSE;

    cap_frame_copy(&state->top, &state->before);
    wheel_scroll(1);

    if (!grab_stable(&state->window_rect, &state->after, &state->scratch)) return FALSE;

    long different = diff_bounds(&state->before, &state->after, &bounds);

    if (different <= state->still_limit)
    {
        log_msg(L"Прокрутка: колесо не сдвинуло список - он помещается целиком (или прокрутка недоступна)");
        return FALSE;
    }

    // изменившаяся область - видимая часть списка
    int scrollbar_margin = GetSystemMetrics(SM_CXVSCROLL) + SCROLLBAR_MARGIN;
    BOOL band_wide = bounds.right - scrollbar_margin > bounds.left + COMPARE_BAND_MIN_WIDTH;

    state->band_left = bounds.left;
    state->band_right = band_wide ? bounds.right - scrollbar_margin : bounds.right;
    state->list_top = bounds.top;
    state->list_bottom = bounds.bottom;
    state->visible_px = state->list_bottom - state->list_top;

    if (!alloc_row_buffers(state, state->before.height))
    {
        undo_scroll(state);
        return FALSE;
    }

    state->notch_px = measure_shift(state, 1, &score);

    BOOL measured = state->notch_px > 0 && state->notch_px < state->visible_px;

    if (!measured) save_debug_frames(&state->before, &state->after, score);

    cap_frame_swap(&state->before, &state->after);
    state->total_notches = 1;

    if (!measured)
    {
        log_msg(L"Прокрутка: не удалось измерить сдвиг (область %d,%d-%d,%d, совпало строк %d%%) - снимаю одну "
                L"страницу", bounds.left, bounds.top, bounds.right, bounds.bottom, score);
        undo_scroll(state);
        return FALSE;
    }

    if (dump)
    {
        log_msg(L"Прокрутка: видимая часть списка %d,%d-%d,%d (%d px), щелчок колеса = %d px",
                state->window_rect.left + bounds.left, state->window_rect.top + bounds.top,
                state->window_rect.left + bounds.right, state->window_rect.top + bounds.bottom, state->visible_px,
                state->notch_px);
    }

    return TRUE;
}

// Шаг 2: крутить до конца по ~60% видимой части за шаг, суммируя измеренные сдвиги.
// Возвращает длину прокрутки S в пикселях.
static int measure_scroll_length(SCROLL_STATE *state, BOOL dump)
{
    int scroll_length = state->notch_px;

    state->notches_per_step = (state->visible_px * STEP_VISIBLE_PERCENT / PERCENT_BASE) / state->notch_px;

    if (state->notches_per_step < 1) state->notches_per_step = 1;

    while (!wnd_aborting() && state->total_notches <= MAX_NOTCHES)
    {
        int shift = scroll_and_measure(state, state->notches_per_step);

        state->total_notches += state->notches_per_step;

        if (shift == 0) break;      // конец списка

        if (shift == SHIFT_UNKNOWN)
        {
            shift = state->notches_per_step * state->notch_px;
            log_msg(L"Прокрутка: сдвиг не измерен, считаю %d px", shift);
        }

        scroll_length += shift;

        if (dump)
        {
            log_msg(L"Прокрутка: +%d щелч. -> сдвиг %d px, всего %d px", state->notches_per_step, shift,
                    scroll_length);
        }
    }

    return scroll_length;
}

// Шаг 3: обратно наверх. FALSE - кадр не снять (снимать страницы нельзя).
static BOOL rewind_to_top(SCROLL_STATE *state)
{
    wheel_scroll(-(state->total_notches + REWIND_EXTRA_NOTCHES));

    if (!grab_stable(&state->window_rect, &state->before, &state->scratch)) return FALSE;

    for (int i = 0; i < TOP_CHECK_MAX_STEPS && !wnd_aborting(); i++)
    {
        long different = cap_frame_diff(&state->before, &state->top);

        if (different != CAP_FRAME_DIFF_INVALID && different <= state->still_limit) break;
        if (scroll_and_measure(state, -state->notches_per_step) == 0) break;   // выше некуда
    }

    return TRUE;
}

// Шаг 4, раскладка: число страниц и шаг между ними; перекрытие - не меньше трети щелчка (~ строка).
static void plan_pages(const SCROLL_STATE *state, int scroll_length, int max_pages, PAGE_PLAN *plan)
{
    int overlap_px = state->notch_px / OVERLAP_NOTCH_DIVISOR;

    if (overlap_px < MIN_OVERLAP_PX) overlap_px = MIN_OVERLAP_PX;

    int page_step = state->visible_px - overlap_px;

    if (page_step < state->notch_px) page_step = state->notch_px;

    plan->page_count = 1 + (scroll_length + page_step - 1) / page_step;
    plan->truncated = FALSE;

    if (plan->page_count > max_pages)
    {
        log_msg(L"Прокрутка: список длиннее %d страниц - сниму первые %d (подряд, с перекрытием)", max_pages,
                max_pages);
        plan->page_count = max_pages;
        plan->truncated = TRUE;
    }

    if (plan->truncated) plan->step_px = page_step;
    else plan->step_px = plan->page_count > 1 ? scroll_length / (plan->page_count - 1) : 0;

    log_msg(L"Прокрутка: видимая часть %d px, длина прокрутки %d px, щелчок %d px -> страниц %d, шаг %d px, "
            L"перекрытие %d px", state->visible_px, scroll_length, state->notch_px, plan->page_count, plan->step_px,
            state->visible_px - plan->step_px);
}

// Прокрутить к смещению target (от начала списка); *offset - текущее смещение. Возвращает,
// на сколько пикселей сдвинулись.
// К цели - частями не больше notches_per_step щелчков (~60% видимой части): у соседних кадров
// остаётся перекрытие, и каждый сдвиг измеряется. Щелчки могут теряться - число щелчков
// считаем по полному щелчку (без перелёта), недолёт добираем в цикле.
static int scroll_to_offset(SCROLL_STATE *state, int target, int *offset)
{
    int moved = 0;
    int half_notch = state->notch_px / 2;

    for (int tries = 0; target - *offset > half_notch && tries < MAX_APPROACH_STEPS && !wnd_aborting(); tries++)
    {
        int notches = (target - *offset + half_notch) / state->notch_px;

        if (notches > state->notches_per_step) notches = state->notches_per_step;
        if (notches < 1) notches = 1;

        int shift = scroll_and_measure(state, notches);

        if (shift == 0) break;      // конец списка

        if (shift == SHIFT_UNKNOWN)
        {
            shift = notches * state->notch_px;
            log_msg(L"Прокрутка: сдвиг не измерен, считаю %d px", shift);
        }

        moved += shift;
        *offset += shift;
    }

    return moved;
}

// Последняя страница - точно конец списка: докрутить по щелчку, пока список двигается.
static int scroll_to_end(SCROLL_STATE *state, int *offset)
{
    int moved = 0;

    for (int i = 0; i < MAX_TAIL_NOTCHES && !wnd_aborting(); i++)
    {
        int shift = scroll_and_measure(state, 1);

        if (shift == 0) break;
        if (shift == SHIFT_UNKNOWN) shift = state->notch_px;

        *offset += shift;
        moved += shift;
    }

    return moved;
}

// Прокрутить к странице page (с нуля). FALSE - кадр не снять, дальше снимать нельзя.
static BOOL move_to_page(SCROLL_STATE *state, const PAGE_PLAN *plan, int page, int scroll_length, int *offset)
{
    BOOL last_page = (page == plan->page_count - 1 && !plan->truncated);
    int target = last_page ? scroll_length : page * plan->step_px;

    if (!grab_stable(&state->window_rect, &state->before, &state->scratch)) return FALSE;

    int moved = scroll_to_offset(state, target, offset);

    if (last_page) moved += scroll_to_end(state, offset);

    log_msg(L"Прокрутка: страница %d - смещение %d px (сдвиг от предыдущей %d px)", page + 1, *offset, moved);

    if (moved > state->visible_px)
    {
        log_msg(L"ВНИМАНИЕ: сдвиг между страницами %d и %d больше видимой части (%d px) - часть списка могла не "
                L"попасть на снимки", page, page + 1, state->visible_px);
    }

    return TRUE;
}

// Шаг 4: страницы с равным шагом. Возвращает, сколько снимков удалось.
static int shoot_pages(SCROLL_STATE *state, int scroll_length, int max_pages, SCROLL_SHOT shot, void *arg,
                       int *page_count_out)
{
    PAGE_PLAN plan;
    int offset = 0;
    int shots_ok = 0;

    plan_pages(state, scroll_length, max_pages, &plan);
    *page_count_out = plan.page_count;

    for (int page = 0; page < plan.page_count && !wnd_aborting(); page++)
    {
        if (page > 0 && !move_to_page(state, &plan, page, scroll_length, &offset)) break;
        if (shot(page + 1, plan.page_count, arg)) shots_ok++;

        SetCursorPos(state->wheel_point.x, state->wheel_point.y);
    }

    return shots_ok;
}

static int shoot_single_page(SCROLL_SHOT shot, void *arg, int *page_count_out)
{
    *page_count_out = 1;

    return shot(1, 1, arg) ? 1 : 0;
}

int scroll_pages(HWND window, int max_pages, BOOL dump, SCROLL_SHOT shot, void *arg, int *page_count_out)
{
    SCROLL_STATE state;
    int shots_ok = 0;

    *page_count_out = 1;
    scroll_state_init(&state, window);

    if (measure_first_notch(&state, dump))
    {
        int scroll_length = measure_scroll_length(&state, dump);

        if (rewind_to_top(&state)) shots_ok = shoot_pages(&state, scroll_length, max_pages, shot, arg, page_count_out);
    }
    else
    {
        shots_ok = shoot_single_page(shot, arg, page_count_out);
    }

    scroll_state_free(&state);

    return shots_ok;
}
