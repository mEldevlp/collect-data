// Специальные возможности Windows (MSAA). См. acc.h.
#include "acc.h"
#include "log.h"

// Название элемента (узла дерева, строки списка). Более длинное обрежется.
#define ACC_NAME_CCH 260

// COINIT_APARTMENTTHREADED из objbase.h (ole32 подгружаем сами - значение берём явно)
#define COINIT_APARTMENTTHREADED_VALUE 2

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

    if (!g_oleacc_loaded) log_msg(L"MSAA: специальные возможности (oleacc) недоступны - окна не прочитать");

    return g_oleacc_loaded;
}

BOOL acc_start(void)
{
    if (!ensure_oleacc()) return FALSE;

    g_co_initialize_ex(NULL, COINIT_APARTMENTTHREADED_VALUE);

    return TRUE;
}

void acc_stop(void)
{
    // acc_start вызывает CoInitializeEx сразу после загрузки oleacc, поэтому CoUninitialize
    // парный ему - по тому же признаку
    if (g_oleacc_loaded && g_co_uninitialize) g_co_uninitialize();
}

IAccessible *acc_window_object(HWND window, const WCHAR *what)
{
    IAccessible *accessible = NULL;

    if (!g_oleacc_loaded) return NULL;

    HRESULT result = g_accessible_object_from_window(window, (DWORD)OBJID_CLIENT, &IID_IAccessible_local,
                                                     (void **)&accessible);

    if (FAILED(result) || !accessible)
    {
        log_msg(L"MSAA: не получить объект доступности: %s (код 0x%08lX)", what, (unsigned long)result);
        return NULL;
    }

    return accessible;
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

// Элемент (ACC_ITEM)

ACC_ITEM acc_item_of(IAccessible *accessible, long child)
{
    ACC_ITEM item;

    item.accessible = accessible;
    item.child = child;

    return item;
}

void acc_item_release(ACC_ITEM *item)
{
    if (item->accessible) IAccessible_Release(item->accessible);

    item->accessible = NULL;
    item->child = CHILDID_SELF;
}

BOOL acc_item_name(const ACC_ITEM *item, WCHAR *name, int name_cch)
{
    VARIANT self = variant_child_id(item->child);
    BSTR text = NULL;

    name[0] = 0;

    HRESULT result = IAccessible_get_accName(item->accessible, self, &text);

    if (SUCCEEDED(result) && text) lstrcpynW(name, text, name_cch);
    if (text && g_sys_free_string) g_sys_free_string(text);

    return name[0] != 0;
}

void acc_item_select(const ACC_ITEM *item, long flags)
{
    VARIANT self = variant_child_id(item->child);

    IAccessible_accSelect(item->accessible, flags, self);
}

long acc_item_state(const ACC_ITEM *item)
{
    VARIANT self = variant_child_id(item->child);
    VARIANT state;
    long value = 0;

    state.vt = VT_EMPTY;

    if (SUCCEEDED(IAccessible_get_accState(item->accessible, self, &state)) && state.vt == VT_I4) value = state.lVal;

    variant_clear_local(&state);

    return value;
}

BOOL acc_item_default_action(const ACC_ITEM *item)
{
    VARIANT self = variant_child_id(item->child);

    return SUCCEEDED(IAccessible_accDoDefaultAction(item->accessible, self));
}

long acc_item_child_count(const ACC_ITEM *item)
{
    long count = 0;

    if (item->child != CHILDID_SELF) return 0;
    if (FAILED(IAccessible_get_accChildCount(item->accessible, &count))) return 0;

    return count < 0 ? 0 : count;
}

// Перенести детей из variant_array (obtained штук) в item_array: объекты - запросом IAccessible,
// простые элементы - как parent с номером элемента. VARIANT освобождаются. Возвращает число элементов.
static int take_children(const ACC_ITEM *parent, VARIANT *variant_array, long obtained, ACC_ITEM *item_array)
{
    int item_count = 0;

    for (long i = 0; i < obtained; i++)
    {
        if (variant_array[i].vt == VT_DISPATCH && variant_array[i].pdispVal)
        {
            IAccessible *child = NULL;

            if (SUCCEEDED(IDispatch_QueryInterface(variant_array[i].pdispVal, &IID_IAccessible_local,
                                                   (void **)&child)) && child)
            {
                item_array[item_count++] = acc_item_of(child, CHILDID_SELF);
            }
        }
        else if (variant_array[i].vt == VT_I4)
        {
            IAccessible_AddRef(parent->accessible);
            item_array[item_count++] = acc_item_of(parent->accessible, variant_array[i].lVal);
        }

        variant_clear_local(&variant_array[i]);
    }

    return item_count;
}

int acc_item_children(const ACC_ITEM *parent, BUFFER *items)
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
    int item_count = 0;

    if (SUCCEEDED(result) && obtained > 0 &&
        buffer_init_array(items, (size_t)obtained, sizeof(ACC_ITEM)) == BUFFER_OK)
    {
        item_count = take_children(parent, variant_array, obtained, items->data);
    }

    for (long i = 0; i < count; i++) variant_clear_local(&variant_array[i]);

    buffer_destroy(&variants);

    return item_count;
}

void acc_items_free(BUFFER *items, int count)
{
    ACC_ITEM *item_array = items->data;

    for (int i = 0; i < count; i++) acc_item_release(&item_array[i]);

    buffer_destroy(items);
}

BOOL acc_first_child(const ACC_ITEM *parent, ACC_ITEM *out)
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

BOOL acc_find_child(const ACC_ITEM *parent, const NAMES_NODE *node, BOOL prefix_allowed, ACC_ITEM *out)
{
    BUFFER items = { 0 };
    int count = acc_item_children(parent, &items);
    ACC_ITEM *item_array = items.data;
    BOOL ok = FALSE;

    for (int i = 0; i < count && !ok; i++)
    {
        WCHAR name[ACC_NAME_CCH];

        if (acc_item_name(&item_array[i], name, ACC_NAME_CCH) && names_match(name, node, prefix_allowed))
        {
            *out = item_array[i];
            item_array[i].accessible = NULL;    // владение передали out
            ok = TRUE;
        }
    }

    acc_items_free(&items, count);

    return ok;
}

void acc_log_children(const ACC_ITEM *parent, const WCHAR *what)
{
    BUFFER items = { 0 };
    int count = acc_item_children(parent, &items);
    ACC_ITEM *item_array = items.data;

    log_msg(L"MSAA: %s (%d):", what, count);

    for (int i = 0; i < count; i++)
    {
        WCHAR name[ACC_NAME_CCH];

        if (acc_item_name(&item_array[i], name, ACC_NAME_CCH)) log_raw(L"    \"%s\"", name);
    }

    acc_items_free(&items, count);
}
