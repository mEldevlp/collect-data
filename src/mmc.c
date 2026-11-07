// Консоль MMC через специальные возможности Windows (MSAA, acc.c). См. mmc.h.
#include "mmc.h"
#include "acc.h"
#include "buffer.h"
#include "common.h"
#include "log.h"
#include "wnd.h"
#include <commctrl.h>
#include <string.h>

// Название узла дерева для журнала (варианты через " / "). Более длинное обрежется.
#define NODE_NAME_CCH 260

// Поиск корня дерева и раскрытие узлов
#define FIND_WINDOW_POLL_MS 150
#define EXPAND_POLL_MS      250     // пауза между нажатиями "вправо" при раскрытии узла

// Список результатов
#define LIST_STABLE_MS         1500     // число строк не менялось столько - список заполнен
#define LIST_POLL_MS           200
#define LIST_SCROLLBAR_GUARD   24       // запас на вертикальную полосу прокрутки при подгонке столбцов
#define LIST_COLUMN_MIN_PX     80       // столбец названия (gpedit) или остатка (lusrmgr) не уже этого
#define LIST_VALUE_COLUMN      1        // gpedit: столбец значения - второй, его показываем целиком
#define LIST_SEARCH_FROM_START (-1)     // LVM_GETNEXTITEM: искать с первой строки
#define MAX_TEXT_COLUMNS       8        // столбцов "по тексту" (lusrmgr - два: имя и полное имя)

// Окно свойств
#define SHEET_MESSAGE_TIMEOUT_MS 3000   // сообщение окну свойств: не ответило - считаем зависшим

// Область действий MMC 3.0 (справа): окно NativeHWNDHost со служебным (не переводится) заголовком
#define ACTION_PANE_CLASS   L"NativeHWNDHost"
#define ACTION_PANE_TITLE   L"ActionsPaneView"
#define ACTION_PANE_WAIT_MS 2000        // после нажатия кнопки область прячется не сразу
#define ACTION_PANE_POLL_MS 100
#define MAX_TOOLBARS        8           // панелей инструментов в окне MMC две: меню и кнопки

static const WCHAR *const PROPERTIES_CLASSES[] = { WND_DIALOG_CLASS, NULL };

// Видимые узлы дерева

// Найти видимый узел по словарю среди прямых детей клиентской области (MSAA показывает дерево плоско:
// все видимые узлы - прямые дети клиентской области). Узлы дерева можно узнавать и по началу названия.
static BOOL find_visible_node(const ACC_ITEM *client, const NAMES_NODE *node, ACC_ITEM *out)
{
    return acc_find_child(client, node, TRUE, out);
}

// Нажимать "вправо" на выделенном узле (только раскрывает, не сворачивает), пока нужный узел не
// станет видимым. Раскрытие ленивое - ждём до timeout_ms.
static BOOL expand_until_visible(const MMC *mmc, const ACC_ITEM *client, const NAMES_NODE *node, DWORD timeout_ms)
{
    DWORD start_tick = GetTickCount();

    for (;;)
    {
        ACC_ITEM found;

        if (find_visible_node(client, node, &found))
        {
            acc_item_release(&found);
            return TRUE;
        }

        if (wnd_aborting() || GetTickCount() - start_tick >= timeout_ms) return FALSE;

        PostMessageW(mmc->tree, WM_KEYDOWN, VK_RIGHT, 0);
        PostMessageW(mmc->tree, WM_KEYUP, VK_RIGHT, 0);
        Sleep(EXPAND_POLL_MS);
    }
}

// Дочернее окно консоли по классу: ждать до timeout_ms.
static HWND wait_descendant(HWND parent, const WCHAR *class_name, DWORD timeout_ms)
{
    DWORD start_tick = GetTickCount();

    for (;;)
    {
        HWND window = wnd_find_descendant(parent, class_name);

        if (window) return window;
        if (wnd_aborting() || GetTickCount() - start_tick >= timeout_ms) return NULL;

        Sleep(FIND_WINDOW_POLL_MS);
    }
}

// Возврат вида консоли (%APPDATA%\Microsoft\MMC\<msc_name>)

#define MMC_VIEW_SUBDIR L"\\Microsoft\\MMC\\"

typedef struct
{
    WCHAR  path[PATH_BUFFER_CCH];
    BUFFER saved;       // прежнее содержимое файла
    BOOL   existed;     // файл был до запуска
    BOOL   active;      // есть что восстанавливать
} CONSOLE_VIEW;

// Консоли в прогоне открываются и закрываются по одной, поэтому хватает одного состояния.
static CONSOLE_VIEW g_console_view;

static BOOL read_file_bytes(const WCHAR *path, BUFFER *out)
{
    buffer_init(out, 0);

    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);

    if (file == INVALID_HANDLE_VALUE) return FALSE;

    DWORD size = GetFileSize(file, NULL);

    if (size == INVALID_FILE_SIZE)
    {
        CloseHandle(file);
        return FALSE;
    }

    if (size > 0)
    {
        DWORD bytes_read = 0;

        if (buffer_init(out, size) != BUFFER_OK || !ReadFile(file, out->data, size, &bytes_read, NULL) ||
            bytes_read != size)
        {
            buffer_destroy(out);
            CloseHandle(file);
            return FALSE;
        }
    }

    CloseHandle(file);

    return TRUE;       // файл был (возможно, пустой)
}

static void write_file_bytes(const WCHAR *path, const BUFFER *content)
{
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

    if (file == INVALID_HANDLE_VALUE) return;

    if (content->size > 0)
    {
        DWORD written = 0;

        WriteFile(file, content->data, (DWORD)content->size, &written, NULL);
    }

    CloseHandle(file);
}

// Действие после закрытия консоли: вернуть файл вида консоли как был (или удалить, если его не было).
static void restore_console_view(void *argument)
{
    CONSOLE_VIEW *view = (CONSOLE_VIEW *)argument;

    if (!view->active) return;

    if (view->existed)
    {
        write_file_bytes(view->path, &view->saved);
        log_msg(L"MMC: вид консоли возвращён как был");
    }
    else
    {
        DeleteFileW(view->path);
        log_msg(L"MMC: файл вида консоли удалён (до запуска его не было)");
    }

    buffer_destroy(&view->saved);
    view->active = FALSE;
}

// Запомнить файл вида консоли до запуска mmc.
static void remember_console_view(const WCHAR *msc_name)
{
    WCHAR appdata[MAX_PATH];

    if (g_console_view.active) buffer_destroy(&g_console_view.saved);     // прошлый не восстановили (/keep)

    ZeroMemory(&g_console_view, sizeof(g_console_view));

    if (!GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH)) return;

    format_text(g_console_view.path, PATH_BUFFER_CCH, L"%s%s%s", appdata, MMC_VIEW_SUBDIR, msc_name);
    g_console_view.existed = read_file_bytes(g_console_view.path, &g_console_view.saved);
    g_console_view.active = TRUE;
}

// Запуск консоли

static const WCHAR *const MMC_FRAME_CLASSES[] = { MMC_FRAME_CLASS, NULL };

HWND mmc_open_console(const WCHAR *msc_name, const WCHAR *log_name, DWORD timeout_ms)
{
    WCHAR command[PATH_BUFFER_CCH];
    WCHAR system_dir[MAX_PATH];

    if (!GetSystemDirectoryW(system_dir, MAX_PATH)) lstrcpyW(system_dir, L"C:\\Windows\\system32");

    format_text(command, PATH_BUFFER_CCH, L"\"%s\\mmc.exe\" \"%s\\%s.msc\"", system_dir, system_dir, msc_name);

    // файл вида консоли читаем до запуска: mmc перепишет его только при закрытии
    remember_console_view(msc_name);

    HWND frame = wnd_open(command, MMC_FRAME_CLASSES, timeout_ms, log_name);

    if (!frame)
    {
        g_console_view.active = FALSE;
        buffer_destroy(&g_console_view.saved);
        return NULL;
    }

    wnd_on_close(frame, restore_console_view, &g_console_view);
    log_msg(L"MMC: вид консоли (%s) будет возвращён после закрытия (%s)", msc_name,
            g_console_view.existed ? L"файл был" : L"файла не было");

    return frame;
}

// Подключение к дереву

BOOL mmc_attach(MMC *mmc, HWND frame, DWORD timeout_ms)
{
    ZeroMemory(mmc, sizeof(*mmc));
    mmc->frame = frame;

    if (!acc_start()) return FALSE;

    mmc->tree = wait_descendant(frame, L"SysTreeView32", timeout_ms);

    if (!mmc->tree)
    {
        log_msg(L"MMC: дерево консоли (SysTreeView32) не найдено");
        return FALSE;
    }

    IAccessible *tree_accessible = acc_window_object(mmc->tree, L"дерево консоли");

    if (!tree_accessible) return FALSE;

    mmc->tree_accessible = tree_accessible;

    // дождаться корневого узла консоли
    ACC_ITEM client = acc_item_of(tree_accessible, CHILDID_SELF);
    DWORD start_tick = GetTickCount();

    while (acc_item_child_count(&client) <= 0)
    {
        if (wnd_aborting() || GetTickCount() - start_tick >= timeout_ms)
        {
            log_msg(L"MMC: корневой узел дерева не появился за %lu мс", timeout_ms);
            return FALSE;
        }

        Sleep(FIND_WINDOW_POLL_MS);
    }

    return TRUE;
}

void mmc_detach(MMC *mmc)
{
    if (mmc->tree_accessible)
    {
        IAccessible_Release((IAccessible *)mmc->tree_accessible);
        mmc->tree_accessible = NULL;
    }

    // в паре с acc_start из mmc_attach - даже если дерево так и не нашлось
    acc_stop();
}

// Навигация

BOOL mmc_select_path(MMC *mmc, const NAMES_PATH *path, DWORD expand_timeout_ms)
{
    if (!mmc->tree_accessible) return FALSE;

    ACC_ITEM client = acc_item_of((IAccessible *)mmc->tree_accessible, CHILDID_SELF);
    ACC_ITEM root;

    // корень консоли ("Политика "Локальный компьютер"", "Локальные пользователи и группы (локально)") -
    // первый видимый узел; раскрываем его, чтобы стал виден первый узел пути (MMC мог раскрыть и сам)
    if (!acc_first_child(&client, &root))
    {
        log_msg(L"MMC: дерево консоли пустое");
        return FALSE;
    }

    acc_item_select(&root, SELFLAG_TAKEFOCUS | SELFLAG_TAKESELECTION);
    BOOL visible = expand_until_visible(mmc, &client, path->nodes[0], expand_timeout_ms);
    acc_item_release(&root);

    BOOL ok = TRUE;

    for (int level = 0; ok && path->nodes[level]; level++)
    {
        ACC_ITEM item;

        // предыдущий шаг раскрывал узел под path->nodes[level]; не стал виден - его нет
        if (!visible || !find_visible_node(&client, path->nodes[level], &item))
        {
            WCHAR wanted[NODE_NAME_CCH];

            names_describe(path->nodes[level], wanted, NODE_NAME_CCH);
            log_msg(L"MMC: узел не найден (%s), путь \"%s\"", wanted, path->title);
            acc_log_children(&client, L"видимые узлы дерева");
            ok = FALSE;
            break;
        }

        // выделить узел; для последнего MMC сразу покажет его содержимое в правой панели
        acc_item_select(&item, SELFLAG_TAKEFOCUS | SELFLAG_TAKESELECTION);

        if (path->nodes[level + 1]) visible = expand_until_visible(mmc, &client, path->nodes[level + 1],
                                                                   expand_timeout_ms);

        acc_item_release(&item);
    }

    if (ok) log_msg(L"MMC: выбран узел \"%s\"", path->title);

    return ok;
}

// Список результатов

static int list_item_count(HWND list)
{
    return (int)SendMessageW(list, LVM_GETITEMCOUNT, 0, 0);
}

BOOL mmc_wait_result_list(MMC *mmc, DWORD timeout_ms)
{
    mmc->list = wait_descendant(mmc->frame, L"SysListView32", timeout_ms);

    if (!mmc->list)
    {
        log_msg(L"MMC: список результатов (SysListView32) не найден");
        return FALSE;
    }

    // ждём, пока число строк перестанет расти
    DWORD start_tick = GetTickCount();
    int previous_count = -1;
    DWORD stable_since = start_tick;

    for (;;)
    {
        int count = list_item_count(mmc->list);

        if (count != previous_count)
        {
            previous_count = count;
            stable_since = GetTickCount();
        }

        if (previous_count > 0 && GetTickCount() - stable_since >= LIST_STABLE_MS) break;
        if (wnd_aborting() || GetTickCount() - start_tick >= timeout_ms) break;

        Sleep(LIST_POLL_MS);
    }

    log_msg(L"MMC: строк в списке: %d", previous_count < 0 ? 0 : previous_count);

    return previous_count > 0;
}

// Ширина столбца при автоподгонке по режиму (LVSCW_AUTOSIZE или LVSCW_AUTOSIZE_USEHEADER).
static int autosize_width(HWND list, int column, int mode)
{
    SendMessageW(list, LVM_SETCOLUMNWIDTH, (WPARAM)column, (LPARAM)mode);

    return (int)SendMessageW(list, LVM_GETCOLUMNWIDTH, (WPARAM)column, 0);
}

// Ширина столбца, при которой видны и текст строк, и заголовок.
static int text_width(HWND list, int column)
{
    // по отдельности: макрос max вычисляет аргументы дважды
    int width_text = autosize_width(list, column, LVSCW_AUTOSIZE);
    int width_header = autosize_width(list, column, LVSCW_AUTOSIZE_USEHEADER);

    return max(width_text, width_header);
}

// Ширина клиентской области списка без запаса на вертикальную полосу прокрутки. 0 - не узнать.
static int list_available_width(HWND list)
{
    RECT client;

    if (!GetClientRect(list, &client)) return 0;

    return client.right - client.left - LIST_SCROLLBAR_GUARD;
}

void mmc_fit_value_column(MMC *mmc)
{
    if (!mmc->list) return;

    int width_name = text_width(mmc->list, 0);
    int width_value = text_width(mmc->list, LIST_VALUE_COLUMN);
    int available = list_available_width(mmc->list);

    if (available <= 0) return;

    if (width_name + width_value > available)
    {
        width_name = available - width_value;

        if (width_name < LIST_COLUMN_MIN_PX) width_name = LIST_COLUMN_MIN_PX;
    }

    SendMessageW(mmc->list, LVM_SETCOLUMNWIDTH, 0, (LPARAM)width_name);
    SendMessageW(mmc->list, LVM_SETCOLUMNWIDTH, LIST_VALUE_COLUMN, (LPARAM)width_value);
    log_msg(L"MMC: ширина столбцов списка: %d и %d (место %d)", width_name, width_value, available);
}

// Число столбцов списка (по заголовку). 0 - не узнать.
static int list_column_count(HWND list)
{
    HWND header = (HWND)SendMessageW(list, LVM_GETHEADER, 0, 0);

    if (!header) return 0;

    int count = (int)SendMessageW(header, HDM_GETITEMCOUNT, 0, 0);

    return count < 0 ? 0 : count;
}

// Сузить текстовые столбцы (кроме первого), начиная с последнего, но не уже LIST_COLUMN_MIN_PX, пока
// вместе с минимумом для следующего столбца (rest_min) они не влезут в available. Возвращает их сумму.
static int shrink_text_columns(int *widths, int text_columns, int rest_min, int available)
{
    int used = 0;

    for (int column = 0; column < text_columns; column++) used += widths[column];

    for (int column = text_columns - 1; column > 0 && used + rest_min > available; column--)
    {
        int narrowed = max(widths[column] - (used + rest_min - available), LIST_COLUMN_MIN_PX);

        if (narrowed >= widths[column]) continue;

        used -= widths[column] - narrowed;
        widths[column] = narrowed;
    }

    return used;
}

void mmc_fit_text_columns(MMC *mmc, int text_columns)
{
    int widths[MAX_TEXT_COLUMNS];

    if (!mmc->list) return;

    int column_count = list_column_count(mmc->list);
    int available = list_available_width(mmc->list);

    if (column_count <= 0 || available <= 0) return;
    if (text_columns > column_count) text_columns = column_count;
    if (text_columns > MAX_TEXT_COLUMNS) text_columns = MAX_TEXT_COLUMNS;

    BOOL has_rest = text_columns < column_count;

    for (int column = 0; column < text_columns; column++) widths[column] = text_width(mmc->list, column);

    // длинный текст (XP: полное имя HelpAssistant, SUPPORT_388945a0) не должен выталкивать следующий
    // столбец за край - первый столбец (имя) всегда целиком, остальные сужаются
    int used = shrink_text_columns(widths, text_columns, has_rest ? LIST_COLUMN_MIN_PX : 0, available);
    int rest = max(available - used, LIST_COLUMN_MIN_PX);

    for (int column = 0; column < text_columns; column++)
    {
        SendMessageW(mmc->list, LVM_SETCOLUMNWIDTH, (WPARAM)column, (LPARAM)widths[column]);
    }

    if (has_rest) SendMessageW(mmc->list, LVM_SETCOLUMNWIDTH, (WPARAM)text_columns, (LPARAM)rest);

    log_msg(L"MMC: столбцов в списке %d, первые %d по тексту - вместе %d, следующему - %d (место %d)", column_count,
            text_columns, used, has_rest ? rest : 0, available);
}

// Окно свойств строки списка

// Окно - окно свойств (с вкладками), а не сообщение об ошибке.
static BOOL is_property_sheet(HWND dialog)
{
    DWORD_PTR result = 0;

    if (!SendMessageTimeoutW(dialog, PSM_GETTABCONTROL, 0, 0, SMTO_ABORTIFHUNG, SHEET_MESSAGE_TIMEOUT_MS, &result))
    {
        return FALSE;
    }

    return result && IsWindow((HWND)result);
}

// Окно с фокусом ввода в потоке окна window. NULL - не узнать.
static HWND thread_focus(HWND window)
{
    GUITHREADINFO info;

    ZeroMemory(&info, sizeof(info));
    info.cbSize = sizeof(info);

    if (!GetGUIThreadInfo(GetWindowThreadProcessId(window, NULL), &info)) return NULL;

    return info.hwndFocus;
}

// Состояние строки списка - в журнал (номера строк - сообщениями без указателей, память mmc не нужна).
// Возвращает состояние MSAA.
static long log_item_state(const MMC *mmc, const ACC_ITEM *item, const WCHAR *item_name, const WCHAR *how)
{
    long state = acc_item_state(item);
    int selected_count = (int)SendMessageW(mmc->list, LVM_GETSELECTEDCOUNT, 0, 0);
    int selected = (int)SendMessageW(mmc->list, LVM_GETNEXTITEM, (WPARAM)LIST_SEARCH_FROM_START, LVNI_SELECTED);
    int focused = (int)SendMessageW(mmc->list, LVM_GETNEXTITEM, (WPARAM)LIST_SEARCH_FROM_START, LVNI_FOCUSED);

    log_msg(L"MMC: строка \"%s\" (элемент MSAA %ld) %s: состояние 0x%lX, выделено строк %d (строка %d), фокус - "
            L"строка %d, фокус ввода %s", item_name, item->child, how, (unsigned long)state, selected_count,
            selected, focused, thread_focus(mmc->list) == mmc->list ? L"в списке" : L"не в списке");

    return state;
}

// Найти строку списка с названием item_name (целиком, без учёта регистра, "ё" = "е") и выделить её -
// в item (с передачей владения). FALSE - строки нет (в журнал - все строки) или она не выделилась.
static BOOL select_list_item(const MMC *mmc, IAccessible *list_accessible, const WCHAR *item_name, ACC_ITEM *item)
{
    ACC_ITEM client = acc_item_of(list_accessible, CHILDID_SELF);
    NAMES_NODE wanted;

    ZeroMemory(&wanted, sizeof(wanted));
    wanted.variants[0] = item_name;

    // только целиком: "Пользователи" не должны найтись как "Пользователи удаленного рабочего стола"
    if (!acc_find_child(&client, &wanted, FALSE, item))
    {
        log_msg(L"MMC: в списке нет строки \"%s\"", item_name);
        acc_log_children(&client, L"строки списка");
        return FALSE;
    }

    // строку с фокусом oleacc выделяет, только когда фокус ввода уже в самом списке (у дерева он есть
    // с запуска консоли; Win7: без этого строка не выделялась) - сначала фокус списку
    acc_item_select(&client, SELFLAG_TAKEFOCUS);
    acc_item_select(item, SELFLAG_TAKEFOCUS | SELFLAG_TAKESELECTION);

    if (log_item_state(mmc, item, item_name, L"с фокусом") & STATE_SYSTEM_SELECTED) return TRUE;

    // запасной путь: выделить без фокуса
    acc_item_select(item, SELFLAG_TAKESELECTION);

    if (log_item_state(mmc, item, item_name, L"без фокуса") & STATE_SYSTEM_SELECTED) return TRUE;

    log_msg(L"MMC: строка \"%s\" не выделилась", item_name);
    acc_item_release(item);

    return FALSE;
}

// Новое окно свойств процесса консоли: ждать до timeout_ms. Открылось сообщение (ошибка, нет прав) -
// закрыть его и вернуть NULL.
static HWND wait_properties(const WND_SET *before, DWORD process_id, DWORD timeout_ms)
{
    WCHAR description[TEXT_BUFFER_CCH];
    HWND dialog = wnd_wait_new(before, process_id, PROPERTIES_CLASSES, timeout_ms);

    if (!dialog || is_property_sheet(dialog)) return dialog;

    wnd_describe(dialog, description, TEXT_BUFFER_CCH);
    log_msg(L"MMC: вместо окна свойств открылось сообщение, закрываю: %s", description);
    PostMessageW(dialog, WM_CLOSE, 0, 0);

    return NULL;
}

// Открыть свойства выделенной строки: Enter в списке (MMC выполняет действие по умолчанию - у групп
// и пользователей это "Свойства"). Не открылось - запасной путь: действие по умолчанию строки через MSAA
// (двойной щелчок по ней; консоль в это время поверх всех - строку не закрывают другие окна).
// before - память под список окон до нажатия.
static HWND press_properties(const MMC *mmc, const ACC_ITEM *item, const WCHAR *item_name, WND_SET *before,
                             DWORD timeout_ms)
{
    DWORD process_id = 0;

    GetWindowThreadProcessId(mmc->frame, &process_id);
    wnd_snapshot(before);
    PostMessageW(mmc->list, WM_KEYDOWN, VK_RETURN, 0);
    PostMessageW(mmc->list, WM_KEYUP, VK_RETURN, 0);

    HWND dialog = wait_properties(before, process_id, timeout_ms);

    if (dialog || wnd_aborting()) return dialog;

    log_msg(L"MMC: Enter не открыл свойства \"%s\" за %lu мс - двойной щелчок по строке (MSAA)", item_name,
            timeout_ms);

    if (!acc_item_default_action(item)) log_msg(L"MMC: действие по умолчанию строки не выполнено");

    return wait_properties(before, process_id, timeout_ms);
}

// Записать окно свойств в журнал и проверить заголовок: в нём должно быть название строки
// ("Свойства: Администраторы", "Administrators Properties").
static void log_properties(HWND dialog, const WCHAR *item_name, const WCHAR *log_name)
{
    WCHAR description[TEXT_BUFFER_CCH];
    WCHAR title[TEXT_BUFFER_CCH];

    wnd_describe(dialog, description, TEXT_BUFFER_CCH);
    log_msg(L"%s: окно %s", log_name, description);
    wnd_title(dialog, title, TEXT_BUFFER_CCH);

    if (!wcsstr(title, item_name))
    {
        log_msg(L"Внимание: в заголовке окна свойств нет \"%s\" - возможно, открылись свойства другой строки",
                item_name);
    }
}

HWND mmc_open_item_properties(MMC *mmc, const WCHAR *item_name, const WCHAR *log_name, DWORD timeout_ms)
{
    BUFFER before = { 0 };
    ACC_ITEM item;
    HWND dialog = NULL;

    if (!mmc->list || wnd_aborting()) return NULL;

    IAccessible *list_accessible = acc_window_object(mmc->list, L"список результатов");

    if (!list_accessible) return NULL;

    // консоль - активная и поверх всех: фокус ввода можно передать списку, а двойной щелчок (запасной
    // путь) попадёт в строку, даже если уже открыто другое окно свойств
    wnd_raise(&mmc->frame, 1);

    // WND_SET большой (WND_SET_MAX окон) - не на стеке
    if (select_list_item(mmc, list_accessible, item_name, &item))
    {
        if (buffer_init(&before, sizeof(WND_SET)) == BUFFER_OK)
        {
            dialog = press_properties(mmc, &item, item_name, before.data, timeout_ms);
        }

        acc_item_release(&item);
    }

    wnd_unraise(&mmc->frame, 1);
    IAccessible_Release(list_accessible);
    buffer_destroy(&before);

    if (!dialog)
    {
        log_msg(L"MMC: окно свойств \"%s\" не открылось", item_name);
        return NULL;
    }

    // окно принадлежит mmc.exe (процесс запомнен с окном консоли) - закрывается только WM_CLOSE
    wnd_track(dialog, NULL, log_name);
    log_properties(dialog, item_name, log_name);

    return dialog;
}

// Область действий (MMC 3.0)

// Части окна консоли: панели инструментов (меню MMC - тоже панель инструментов) и область действий
typedef struct
{
    HWND action_pane;                   // NULL - нет (MMC 2.0 на XP)
    HWND toolbars[MAX_TOOLBARS];
    int  toolbar_count;
} FRAME_PARTS;

static BOOL CALLBACK frame_parts_proc(HWND window, LPARAM param)
{
    FRAME_PARTS *parts = (FRAME_PARTS *)param;
    WCHAR class_name[CLASS_NAME_CCH];
    WCHAR title[CLASS_NAME_CCH];

    if (!GetClassNameW(window, class_name, CLASS_NAME_CCH)) return TRUE;

    if (lstrcmpiW(class_name, TOOLBARCLASSNAMEW) == 0)
    {
        if (parts->toolbar_count < MAX_TOOLBARS) parts->toolbars[parts->toolbar_count++] = window;
    }
    else if (lstrcmpiW(class_name, ACTION_PANE_CLASS) == 0)
    {
        wnd_title(window, title, CLASS_NAME_CCH);

        if (lstrcmpW(title, ACTION_PANE_TITLE) == 0) parts->action_pane = window;
    }

    return TRUE;
}

static void find_frame_parts(HWND frame, FRAME_PARTS *parts)
{
    ZeroMemory(parts, sizeof(*parts));
    EnumChildWindows(frame, frame_parts_proc, (LPARAM)parts);
}

// Нажать кнопку панели инструментов, название которой подходит под button_name (MSAA, действие
// по умолчанию). FALSE - такой кнопки нет.
static BOOL press_toolbar_button(const FRAME_PARTS *parts, const NAMES_NODE *button_name)
{
    for (int i = 0; i < parts->toolbar_count; i++)
    {
        IAccessible *toolbar = acc_window_object(parts->toolbars[i], L"панель инструментов");

        if (!toolbar) continue;

        ACC_ITEM client = acc_item_of(toolbar, CHILDID_SELF);
        ACC_ITEM button;
        BOOL found = acc_find_child(&client, button_name, TRUE, &button);

        if (found)
        {
            if (!acc_item_default_action(&button)) log_msg(L"MMC: кнопка панели инструментов не нажалась");

            acc_item_release(&button);
        }

        IAccessible_Release(toolbar);

        if (found) return TRUE;
    }

    return FALSE;
}

// Названия кнопок всех панелей инструментов - в журнал (по ним дополняется словарь names.c).
static void log_toolbar_buttons(const FRAME_PARTS *parts)
{
    for (int i = 0; i < parts->toolbar_count; i++)
    {
        IAccessible *toolbar = acc_window_object(parts->toolbars[i], L"панель инструментов");

        if (!toolbar) continue;

        ACC_ITEM client = acc_item_of(toolbar, CHILDID_SELF);

        acc_log_children(&client, L"кнопки панели инструментов");
        IAccessible_Release(toolbar);
    }
}

static BOOL wait_hidden(HWND window, DWORD timeout_ms)
{
    DWORD start_tick = GetTickCount();

    while (IsWindowVisible(window))
    {
        if (wnd_aborting() || GetTickCount() - start_tick >= timeout_ms) return FALSE;

        Sleep(ACTION_PANE_POLL_MS);
    }

    return TRUE;
}

void mmc_hide_action_pane(MMC *mmc)
{
    FRAME_PARTS parts;
    WCHAR wanted[NODE_NAME_CCH];

    find_frame_parts(mmc->frame, &parts);

    if (!parts.action_pane || !IsWindowVisible(parts.action_pane)) return;    // MMC 2.0 или уже скрыта

    // консоль поверх всех: MSAA может нажать кнопку и щелчком мыши
    wnd_raise(&mmc->frame, 1);

    BOOL pressed = press_toolbar_button(&parts, &g_names_action_pane_button);
    BOOL hidden = pressed && wait_hidden(parts.action_pane, ACTION_PANE_WAIT_MS);

    wnd_unraise(&mmc->frame, 1);

    if (hidden)
    {
        log_msg(L"MMC: область действий скрыта");
        return;
    }

    if (pressed)
    {
        log_msg(L"MMC: кнопка нажата, но область действий не скрылась за %lu мс", (unsigned long)ACTION_PANE_WAIT_MS);
        return;
    }

    names_describe(&g_names_action_pane_button, wanted, NODE_NAME_CCH);
    log_msg(L"MMC: нет кнопки %s - область действий остаётся", wanted);
    log_toolbar_buttons(&parts);
}

// Диагностика (/dump)

static BOOL CALLBACK dump_window_proc(HWND window, LPARAM param)
{
    WCHAR description[TEXT_BUFFER_CCH];

    (void)param;
    wnd_describe(window, description, TEXT_BUFFER_CCH);
    log_raw(L"    %s", description);

    return TRUE;
}

void mmc_dump(MMC *mmc)
{
    FRAME_PARTS parts;

    log_msg(L"MMC: дочерние окна консоли:");
    EnumChildWindows(mmc->frame, dump_window_proc, 0);

    if (!mmc->tree_accessible) return;

    find_frame_parts(mmc->frame, &parts);
    log_toolbar_buttons(&parts);

    ACC_ITEM client = acc_item_of((IAccessible *)mmc->tree_accessible, CHILDID_SELF);

    acc_log_children(&client, L"видимые узлы дерева");

    IAccessible *list_accessible = mmc->list ? acc_window_object(mmc->list, L"список результатов") : NULL;

    if (!list_accessible) return;

    ACC_ITEM list_client = acc_item_of(list_accessible, CHILDID_SELF);

    acc_log_children(&list_client, L"строки списка");
    IAccessible_Release(list_accessible);
}
