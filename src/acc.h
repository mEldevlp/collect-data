// Специальные возможности Windows (MSAA, oleacc.dll): элементы чужих окон (узлы дерева, строки списка)
// сами сообщают свои названия и состояние и сами дают себя выделить - в память чужого процесса ничего
// не пишется и не читается. MSAA работает и при разной разрядности процессов (вызовы переносит система).
// oleacc/ole32/oleaut32 подгружаются через LoadLibrary, чтобы список статических импортов exe
// (KERNEL32/USER32/GDI32/ADVAPI32) не менялся.
#ifndef SCREENS_ACC_H
#define SCREENS_ACC_H

#include <windows.h>
#define COBJMACROS
#include <oleacc.h>
#include "buffer.h"
#include "names.h"

// Элемент для MSAA: объект IAccessible и номер элемента внутри него. Обычно элемент - это сам объект
// (child = CHILDID_SELF); строки списка и узлы дерева возвращаются как "простые" элементы родителя
// (child > 0).
typedef struct
{
    IAccessible *accessible;
    long         child;
} ACC_ITEM;

// Подгрузить oleacc/ole32/oleaut32 (один раз) и вызвать CoInitializeEx. FALSE - MSAA недоступна
// (причина в журнале). acc_stop - в паре с каждым вызовом acc_start, даже неудачным.
BOOL acc_start(void);
void acc_stop(void);

// Объект доступности клиентской области окна (what - название окна для журнала). NULL - не получить.
// Освобождать IAccessible_Release.
IAccessible *acc_window_object(HWND window, const WCHAR *what);

// Элемент из объекта и номера элемента (без составного инициализатора - он на /W4 даёт C4204).
ACC_ITEM acc_item_of(IAccessible *accessible, long child);
void acc_item_release(ACC_ITEM *item);

// Название элемента. FALSE - имени нет.
BOOL acc_item_name(const ACC_ITEM *item, WCHAR *name, int name_cch);

// Выделить элемент (flags - SELFLAG_*; с TAKEFOCUS - передать ему фокус).
void acc_item_select(const ACC_ITEM *item, long flags);

// Состояние элемента (STATE_SYSTEM_*: выделен, в фокусе...). 0 - не узнать.
long acc_item_state(const ACC_ITEM *item);

// Действие по умолчанию (у строки списка - "двойной щелчок"). FALSE - не выполнено.
BOOL acc_item_default_action(const ACC_ITEM *item);

// Число дочерних элементов (0 - нет или элемент простой).
long acc_item_child_count(const ACC_ITEM *item);

// Дочерние элементы parent в массив ACC_ITEM items (выделяется здесь, освобождать acc_items_free).
// Возвращает их число.
int acc_item_children(const ACC_ITEM *parent, BUFFER *items);
void acc_items_free(BUFFER *items, int count);

// Первый дочерний элемент parent в out (с передачей владения). FALSE - детей нет.
BOOL acc_first_child(const ACC_ITEM *parent, ACC_ITEM *out);

// Найти среди детей parent элемент, название которого подходит под node (names_match), в out
// (с передачей владения). prefix_allowed - достаточно совпадения по началу названия.
BOOL acc_find_child(const ACC_ITEM *parent, const NAMES_NODE *node, BOOL prefix_allowed, ACC_ITEM *out);

// Названия всех детей parent - в журнал (what - что это: "видимые узлы дерева", "строки списка").
void acc_log_children(const ACC_ITEM *parent, const WCHAR *what);

#endif
