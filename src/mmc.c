// Консоль MMC через специальные возможности Windows (MSAA). См. mmc.h.
#include "mmc.h"
#include "buffer.h"
#include "common.h"
#include "log.h"
#include "wnd.h"
#define COBJMACROS
#include <oleacc.h>
#include <commctrl.h>

// Название узла дерева. Узлы gpedit короче; более длинное обрежется.
#define NODE_NAME_CCH 260

// Поиск корня дерева и раскрытие узлов
#define FIND_WINDOW_POLL_MS 150
#define EXPAND_POLL_MS      250     // пауза между нажатиями "вправо" при раскрытии узла

// Список результатов
#define LIST_STABLE_MS       1500   // число строк не менялось столько - список заполнен
#define LIST_POLL_MS         200
#define LIST_SCROLLBAR_GUARD 24     // запас на вертикальную полосу прокрутки при подгонке столбцов
#define LIST_COLUMN_MIN_PX   80     // столбец названия не сужаем меньше этого
#define LIST_COLUMNS         2      // в политиках безопасности два столбца: название и значение
#define LIST_VALUE_COLUMN    1      // столбец значения - второй, его показываем целиком

// COINIT_APARTMENTTHREADED из objbase.h (ole32 подгружаем сами - значение берём явно)
#define COINIT_APARTMENTTHREADED_VALUE 2

// Узел дерева для MSAA: объект IAccessible и номер элемента внутри него. Обычный узел - это сам
// объект (child = CHILDID_SELF); иногда узел возвращается как "простой" элемент родителя (child > 0).
typedef struct
{
    IAccessible *accessible;
    long         child;
} ACC_ITEM;

// IID_IAccessible задаём сами, чтобы не тянуть uuid.lib (и не добавлять статический импорт).
static const GUID IID_IAccessible_local =
    { 0x618736E0, 0x3C3D, 0x11CF, { 0x81, 0x0C, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };

typedef HRESULT (WINAPI *PFN_AccessibleObjectFromWindow)(HWND, DWORD, REFIID, void **);
typedef HRESULT (WINAPI *PFN_AccessibleChildren)(IAccessible *, LONG, LONG, VARIANT *, LONG *);
typedef HRESULT (WINAPI *PFN_CoInitializeEx)(LPVOID, DWORD);
typedef void    (WINAPI *PFN_CoUninitialize)(void);
typedef void    (WINAPI *PFN_SysFreeString)(BSTR);

static PFN_AccessibleObjectFromWindow g_accessible_object_from_window = NULL;
static PFN_AccessibleChildren         g_accessible_children = NULL;
static PFN_CoInitializeEx             g_co_initialize_ex = NULL;
static PFN_CoUninitialize             g_co_uninitialize = NULL;
static PFN_SysFreeString              g_sys_free_string = NULL;
static BOOL                           g_oleacc_loaded = FALSE;

// Подгрузить oleacc/ole32/oleaut32 один раз. FALSE - чего-то нет (MSAA недоступна).
static BOOL ensure_oleacc(void)
{
    static BOOL tried = FALSE;

    if (tried) return g_oleacc_loaded;

    tried = TRUE;

    HMODULE oleacc = LoadLibraryW(L"oleacc.dll");
    HMODULE ole32 = LoadLibraryW(L"ole32.dll");
    HMODULE oleaut32 = LoadLibraryW(L"oleaut32.dll");

    if (oleacc)
    {
        g_accessible_object_from_window =
            (PFN_AccessibleObjectFromWindow)GetProcAddress(oleacc, "AccessibleObjectFromWindow");
        g_accessible_children = (PFN_AccessibleChildren)GetProcAddress(oleacc, "AccessibleChildren");
    }

    if (ole32)
    {
        g_co_initialize_ex = (PFN_CoInitializeEx)GetProcAddress(ole32, "CoInitializeEx");
        g_co_uninitialize = (PFN_CoUninitialize)GetProcAddress(ole32, "CoUninitialize");
    }

    if (oleaut32) g_sys_free_string = (PFN_SysFreeString)GetProcAddress(oleaut32, "SysFreeString");

    g_oleacc_loaded = g_accessible_object_from_window && g_accessible_children && g_co_initialize_ex &&
                      g_co_uninitialize && g_sys_free_string;

    if (!g_oleacc_loaded) log_msg(L"MMC: специальные возможности (oleacc) недоступны - дерево не прочитать");

    return g_oleacc_loaded;
}

// VARIANT

static VARIANT variant_child_id(long child)
{
    VARIANT value;

    value.vt = VT_I4;
    value.lVal = child;

    return value;
}

static void variant_clear_local(VARIANT *value)
{
    if (value->vt == VT_DISPATCH && value->pdispVal) IDispatch_Release(value->pdispVal);
    else if (value->vt == VT_UNKNOWN && value->punkVal) IUnknown_Release(value->punkVal);
    else if (value->vt == VT_BSTR && value->bstrVal && g_sys_free_string) g_sys_free_string(value->bstrVal);

    value->vt = VT_EMPTY;
}

// Узел дерева (ACC_ITEM)

static void acc_item_release(ACC_ITEM *item)
{
    if (item->accessible) IAccessible_Release(item->accessible);

    item->accessible = NULL;
    item->child = CHILDID_SELF;
}

// Узел из объекта и номера элемента (без составного инициализатора - он на /W4 даёт C4204).
static ACC_ITEM acc_item_of(IAccessible *accessible, long child)
{
    ACC_ITEM item;

    item.accessible = accessible;
    item.child = child;

    return item;
}

// Название узла. FALSE - имени нет.
static BOOL acc_item_name(const ACC_ITEM *item, WCHAR *name, int name_cch)
{
    VARIANT self = variant_child_id(item->child);
    BSTR text = NULL;

    name[0] = 0;

    HRESULT result = IAccessible_get_accName(item->accessible, self, &text);

    if (SUCCEEDED(result) && text) lstrcpynW(name, text, name_cch);
    if (text && g_sys_free_string) g_sys_free_string(text);

    return name[0] != 0;
}

// Выделить узел (и, с TAKEFOCUS, передать ему фокус).
static void acc_item_select(const ACC_ITEM *item, long flags)
{
    VARIANT self = variant_child_id(item->child);

    IAccessible_accSelect(item->accessible, flags, self);
}

// Число раскрытых дочерних узлов (0 - узел свёрнут или листовой).
static long acc_item_child_count(const ACC_ITEM *item)
{
    long count = 0;

    if (item->child != CHILDID_SELF) return 0;
    if (FAILED(IAccessible_get_accChildCount(item->accessible, &count))) return 0;

    return count < 0 ? 0 : count;
}

// Дочерние узлы parent в массив items (выделяется здесь, освобождать acc_items_free).
// Возвращает их число.
static int acc_item_children(const ACC_ITEM *parent, BUFFER *items)
{
    buffer_init(items, 0);

    long count = acc_item_child_count(parent);

    if (count <= 0) return 0;

    BUFFER variants = { 0 };

    if (buffer_init_array(&variants, (size_t)count, sizeof(VARIANT)) != BUFFER_OK) return 0;

    VARIANT *variant_array = variants.data;

    for (long i = 0; i < count; i++) variant_array[i].vt = VT_EMPTY;

    long obtained = 0;
    HRESULT result = g_accessible_children(parent->accessible, 0, count, variant_array, &obtained);

    if (FAILED(result) || obtained <= 0 ||
        buffer_init_array(items, (size_t)obtained, sizeof(ACC_ITEM)) != BUFFER_OK)
    {
        for (long i = 0; i < count; i++) variant_clear_local(&variant_array[i]);

        buffer_destroy(&variants);

        return 0;
    }

    ACC_ITEM *item_array = items->data;
    int item_count = 0;

    for (long i = 0; i < obtained; i++)
    {
        if (variant_array[i].vt == VT_DISPATCH && variant_array[i].pdispVal)
        {
            IAccessible *child = NULL;

            if (SUCCEEDED(IDispatch_QueryInterface(variant_array[i].pdispVal, &IID_IAccessible_local,
                                                   (void **)&child)) && child)
            {
                item_array[item_count].accessible = child;
                item_array[item_count].child = CHILDID_SELF;
                item_count++;
            }
        }
        else if (variant_array[i].vt == VT_I4)
        {
            IAccessible_AddRef(parent->accessible);
            item_array[item_count].accessible = parent->accessible;
            item_array[item_count].child = variant_array[i].lVal;
            item_count++;
        }

        variant_clear_local(&variant_array[i]);
    }

    buffer_destroy(&variants);

    return item_count;
}

static void acc_items_free(BUFFER *items, int count)
{
    ACC_ITEM *item_array = items->data;

    for (int i = 0; i < count; i++) acc_item_release(&item_array[i]);

    buffer_destroy(items);
}

// Первый дочерний узел parent в out (с передачей владения). FALSE - детей нет.
static BOOL acc_first_child(const ACC_ITEM *parent, ACC_ITEM *out)
{
    BUFFER items = { 0 };
    int count = acc_item_children(parent, &items);
    ACC_ITEM *item_array = items.data;
    BOOL ok = FALSE;

    if (count > 0)
    {
        *out = item_array[0];
        item_array[0].accessible = NULL;        // владение передали out
        ok = TRUE;
    }

    acc_items_free(&items, count);

    return ok;
}

// Найти среди детей parent узел, подходящий под node, в out (с передачей владения).
static BOOL acc_find_child(const ACC_ITEM *parent, const NAMES_NODE *node, ACC_ITEM *out)
{
    BUFFER items = { 0 };
    int count = acc_item_children(parent, &items);
    ACC_ITEM *item_array = items.data;
    BOOL ok = FALSE;

    for (int i = 0; i < count && !ok; i++)
    {
        WCHAR name[NODE_NAME_CCH];

        if (acc_item_name(&item_array[i], name, NODE_NAME_CCH) && names_match(name, node, TRUE))
        {
            *out = item_array[i];
            item_array[i].accessible = NULL;
            ok = TRUE;
        }
    }

    acc_items_free(&items, count);

    return ok;
}

// Названия всех видимых узлов дерева - в журнал (узел не найден: по ним дополняется словарь names.c).
// MSAA показывает дерево плоско: видимые узлы - прямые дети клиентской области дерева.
static void log_visible_nodes(const ACC_ITEM *client)
{
    BUFFER items = { 0 };
    int count = acc_item_children(client, &items);
    ACC_ITEM *item_array = items.data;

    log_msg(L"MMC: видимые узлы дерева (%d):", count);

    for (int i = 0; i < count; i++)
    {
        WCHAR name[NODE_NAME_CCH];

        if (acc_item_name(&item_array[i], name, NODE_NAME_CCH)) log_raw(L"    \"%s\"", name);
    }

    acc_items_free(&items, count);
}

// Найти видимый узел по словарю среди прямых детей клиентской области (плоский список видимых узлов).
static BOOL find_visible_node(const ACC_ITEM *client, const NAMES_NODE *node, ACC_ITEM *out)
{
    return acc_find_child(client, node, out);
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

// Поиск дочернего окна по классу

typedef struct
{
    const WCHAR *class_name;
    HWND         found;
} FIND_WINDOW;

static BOOL CALLBACK find_by_class_proc(HWND window, LPARAM param)
{
    FIND_WINDOW *search = (FIND_WINDOW *)param;
    WCHAR class_name[CLASS_NAME_CCH];

    if (GetClassNameW(window, class_name, CLASS_NAME_CCH) && lstrcmpiW(class_name, search->class_name) == 0)
    {
        search->found = window;
        return FALSE;
    }

    return TRUE;
}

// EnumChildWindows обходит всех потомков (не только прямых детей).
static HWND find_descendant(HWND parent, const WCHAR *class_name)
{
    FIND_WINDOW search;

    search.class_name = class_name;
    search.found = NULL;
    EnumChildWindows(parent, find_by_class_proc, (LPARAM)&search);

    return search.found;
}

static HWND wait_descendant(HWND parent, const WCHAR *class_name, DWORD timeout_ms)
{
    DWORD start_tick = GetTickCount();

    for (;;)
    {
        HWND window = find_descendant(parent, class_name);

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

// gpedit в прогоне открывается и закрывается по одному, поэтому хватает одного состояния.
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

// Действие после закрытия gpedit: вернуть файл вида консоли как был (или удалить, если его не было).
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

    if (!ensure_oleacc()) return FALSE;

    g_co_initialize_ex(NULL, COINIT_APARTMENTTHREADED_VALUE);

    mmc->tree = wait_descendant(frame, L"SysTreeView32", timeout_ms);

    if (!mmc->tree)
    {
        log_msg(L"MMC: дерево консоли (SysTreeView32) не найдено");
        return FALSE;
    }

    IAccessible *tree_accessible = NULL;
    HRESULT result = g_accessible_object_from_window(mmc->tree, (DWORD)OBJID_CLIENT, &IID_IAccessible_local,
                                                     (void **)&tree_accessible);

    if (FAILED(result) || !tree_accessible)
    {
        log_msg(L"MMC: не получить объект доступности дерева (код 0x%08lX)", (unsigned long)result);
        return FALSE;
    }

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

    // mmc_attach вызывает CoInitializeEx сразу после загрузки oleacc, поэтому CoUninitialize
    // парный ему - по тому же признаку, даже если дерево так и не нашлось
    if (g_oleacc_loaded && g_co_uninitialize) g_co_uninitialize();
}

// Навигация

BOOL mmc_select_path(MMC *mmc, const NAMES_PATH *path, DWORD expand_timeout_ms)
{
    if (!mmc->tree_accessible) return FALSE;

    ACC_ITEM client = acc_item_of((IAccessible *)mmc->tree_accessible, CHILDID_SELF);
    ACC_ITEM root;

    // корень консоли ("Политика "Локальный компьютер"" / "Local Computer Policy") - первый видимый
    // узел; раскрываем его, чтобы стал виден первый узел пути (MMC мог раскрыть и сам)
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
            log_msg(L"MMC: узел не найден (%s), сценарий \"%s\" пропущен", wanted, path->title);
            log_visible_nodes(&client);
            ok = FALSE;
            break;
        }

        // выделить узел; для последнего MMC сразу покажет его политики в правой панели
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

// Ширина столбца при автоподгонке по режиму (LVSCW_AUTOSIZE или LVSCW_AUTOSIZE_USEHEADER).
static int autosize_width(HWND list, int column, int mode)
{
    SendMessageW(list, LVM_SETCOLUMNWIDTH, (WPARAM)column, (LPARAM)mode);

    return (int)SendMessageW(list, LVM_GETCOLUMNWIDTH, (WPARAM)column, 0);
}

// Подогнать столбцы под содержимое. Названия и значения видны целиком; если оба не влезают в ширину
// списка - значение (второй столбец) показываем полностью, столбец названия сужаем (но не до нуля),
// чтобы не было горизонтальной прокрутки, которая прячет именно значение (оно справа).
static void fit_result_columns(HWND list)
{
    int width_name = max(autosize_width(list, 0, LVSCW_AUTOSIZE),
                         autosize_width(list, 0, LVSCW_AUTOSIZE_USEHEADER));
    int width_value = max(autosize_width(list, LIST_VALUE_COLUMN, LVSCW_AUTOSIZE),
                          autosize_width(list, LIST_VALUE_COLUMN, LVSCW_AUTOSIZE_USEHEADER));
    RECT client;

    if (!GetClientRect(list, &client)) return;

    int available = client.right - client.left - LIST_SCROLLBAR_GUARD;

    if (width_name + width_value > available)
    {
        width_name = available - width_value;

        if (width_name < LIST_COLUMN_MIN_PX) width_name = LIST_COLUMN_MIN_PX;
    }

    SendMessageW(list, LVM_SETCOLUMNWIDTH, 0, (LPARAM)width_name);
    SendMessageW(list, LVM_SETCOLUMNWIDTH, LIST_VALUE_COLUMN, (LPARAM)width_value);
    log_msg(L"MMC: ширина столбцов списка: %d и %d (место %d)", width_name, width_value, available);
}

BOOL mmc_show_result_list(MMC *mmc, DWORD timeout_ms)
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
    fit_result_columns(mmc->list);

    return previous_count > 0;
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
    log_msg(L"MMC: дочерние окна консоли:");
    EnumChildWindows(mmc->frame, dump_window_proc, 0);

    if (!mmc->tree_accessible) return;

    ACC_ITEM client = acc_item_of((IAccessible *)mmc->tree_accessible, CHILDID_SELF);

    log_visible_nodes(&client);
}
