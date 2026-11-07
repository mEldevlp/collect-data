// Консоль MMC (gpedit.msc; на этапе 5 - lusrmgr.msc): запуск с возвратом вида консоли пользователя,
// навигация по дереву (SysTreeView32) и показ списка результатов (SysListView32).
//
// Дерево читается через специальные возможности Windows (MSAA, oleacc.dll): узлы сами сообщают свои
// названия и сами дают себя выделить - в память процесса mmc.exe ничего не пишется и не читается.
// MSAA работает и при разной разрядности процессов (вызовы переносит система), поэтому отдельная
// сборка под разрядность mmc тут не нужна. oleacc/ole32/oleaut32 подгружаются через LoadLibrary,
// чтобы список статических импортов exe (KERNEL32/USER32/GDI32/ADVAPI32/SHELL32) не менялся.
#ifndef SCREENS_MMC_H
#define SCREENS_MMC_H

#include <windows.h>
#include "names.h"

#define MMC_FRAME_CLASS L"MMCMainFrame"     // главное окно mmc.exe

typedef struct
{
    HWND  frame;                // главное окно консоли
    HWND  tree;                 // дерево консоли (левая панель); NULL - не найдено
    HWND  list;                 // список результатов (правая панель); заполняется в mmc_show_result_list
    void *tree_accessible;      // IAccessible клиентской области дерева (храним как void*, чтобы не
                                // тянуть oleacc.h в заголовок); NULL - дерево не прочитать
} MMC;

// Открыть консоль <system32>\<msc_name>.msc в mmc.exe и дождаться окна MMCMainFrame (окно запоминается
// для закрытия). MMC при закрытии сохраняет вид консоли в профиль пользователя
// (%APPDATA%\Microsoft\MMC\<msc_name>: положение окна, ширина столбцов) - после закрытия этот файл
// возвращается как был (или удаляется, если его не было). NULL - окно не появилось.
HWND mmc_open_console(const WCHAR *msc_name, const WCHAR *log_name, DWORD timeout_ms);

// Подключиться к дереву консоли: загрузить oleacc, найти SysTreeView32, дождаться корневого узла
// (до timeout_ms) и получить его IAccessible. FALSE - причина в журнале; mmc_detach безопасен всегда.
BOOL mmc_attach(MMC *mmc, HWND frame, DWORD timeout_ms);
void mmc_detach(MMC *mmc);

// Пройти путь от корня консоли, раскрывая узлы, и выделить последний (MMC покажет его содержимое
// в правой панели). FALSE - узел не найден (в журнал - названия узлов того уровня) или не раскрылся
// за expand_timeout_ms.
BOOL mmc_select_path(MMC *mmc, const NAMES_PATH *path, DWORD expand_timeout_ms);

// Список результатов (правая панель): дождаться, пока число строк перестанет расти, подогнать ширину
// столбцов под текст (не влезают оба - значение показываем целиком, столбец названия сужаем), число
// строк - в журнал. FALSE - списка нет.
BOOL mmc_show_result_list(MMC *mmc, DWORD timeout_ms);

// /dump: дочерние окна консоли и раскрытое дерево узлов - в журнал.
void mmc_dump(MMC *mmc);

#endif
