// Словарь названий узлов дерева MMC на разных языках интерфейса (RU, EN).
// Узел ищется по любому из вариантов: без учёта регистра, "ё" = "е", сначала точное совпадение,
// затем по началу строки. Языка нет в словаре - узел не найдётся, в журнал попадут названия всех
// узлов уровня (с /dump - всё раскрытое дерево): по ним словарь и дополняется.
#ifndef SCREENS_NAMES_H
#define SCREENS_NAMES_H

#include <windows.h>

#define NAMES_MAX_VARIANTS 4    // вариантов названия одного узла (языки, версии Windows)
#define NAMES_MAX_DEPTH    8    // узлов в пути от корня, вместе с завершающим NULL

typedef struct
{
    const WCHAR *variants[NAMES_MAX_VARIANTS];  // неиспользуемые - NULL
} NAMES_NODE;

// Путь от корня дерева консоли (сам корень не входит): nodes[0] - ребёнок корня, последний - цель.
typedef struct
{
    const WCHAR      *title;                    // для журнала
    const NAMES_NODE *nodes[NAMES_MAX_DEPTH];   // после последнего узла - NULL
} NAMES_PATH;

// gpedit.msc: Конфигурация компьютера - Конфигурация Windows - Параметры безопасности - ...
extern const NAMES_PATH g_names_audit_policy;       // ... - Локальные политики - Политика аудита
extern const NAMES_PATH g_names_password_policy;    // ... - Политики учётных записей - Политика паролей
extern const NAMES_PATH g_names_lockout_policy;     // ... - Политики учётных записей - Политика блокировки

// lusrmgr.msc: Локальные пользователи и группы (локально) - ...
extern const NAMES_PATH g_names_local_users;        // ... - Пользователи
extern const NAMES_PATH g_names_local_groups;       // ... - Группы

// Кнопка "Показать/скрыть область действий" на панели инструментов MMC 3.0
extern const NAMES_NODE g_names_action_pane_button;

// Совпадает ли text с одним из вариантов узла. prefix_allowed - достаточно, чтобы text начинался
// с варианта.
BOOL names_match(const WCHAR *text, const NAMES_NODE *node, BOOL prefix_allowed);

// Варианты названия узла через " / " - для журнала.
void names_describe(const NAMES_NODE *node, WCHAR *text, int text_cch);

#endif
