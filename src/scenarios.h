// Таблица сценариев (что снимаем).
#ifndef SCREENS_SCENARIOS_H
#define SCREENS_SCENARIOS_H

#include <windows.h>
#include "osinfo.h"

// Общие условия всех сценариев прогона
typedef struct
{
    const WCHAR  *results_dir;  // папка результатов
    BOOL          all_monitors;
    BOOL          keep;         // не закрывать окна
    BOOL          dump;         // подробный дамп окон в журнал
    DWORD         wait_ms;      // сколько ждать заполнения окна (список программ и т. п.)
    int           max_pages;    // не больше стольких страниц у длинного списка ПО
    const OSINFO *os_info;
} SCEN_CONTEXT;

struct SCENARIO;
typedef BOOL (*SCEN_RUN)(const SCEN_CONTEXT *context, const struct SCENARIO *scenario);

typedef struct SCENARIO
{
    const WCHAR *id;            // имя для /only
    const WCHAR *file_name;     // имя PNG в папке результатов
    const WCHAR *title;         // описание для /list и журнала
    BOOL         hidden;        // только по /only (служебные)
    SCEN_RUN     run;           // NULL - пока не реализован
} SCENARIO;

extern const SCENARIO g_scenarios[];
extern const int g_scenario_count;

// Общий шаг: окна windows (последнее - самое верхнее) поверх всех, подсказка у часов,
// снимок экрана в <results_dir>\<file_name>, снять "поверх всех".
BOOL scen_shoot(const SCEN_CONTEXT *context, const WCHAR *file_name, HWND *windows, int count);

// Окно сведений о системе: открывается при первом вызове и остаётся до конца.
// NULL - открыть не удалось (снимаем без него).
HWND scen_sysinfo(const SCEN_CONTEXT *context);

#endif
