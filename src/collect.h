// Сбор данных для аудита ИБ - то, что делал collect_audit.cmd: таблица разделов, подготовка папки результатов,
// журнал сбора (_журнал_сбора.txt, открывает main.c), файл команд _команды_сбора.txt, помощники разделов.
// Разделы - в sect_*.c (sections.h). Что и как переносится - collect_plan.md.
#ifndef SCREENS_COLLECT_H
#define SCREENS_COLLECT_H

#include <windows.h>
#include "osinfo.h"
#include "outfile.h"

// Ключи командной строки, от которых зависит сбор
typedef struct
{
    const WCHAR  *results_dir;
    const WCHAR  *folder_note;  // почему взята запасная папка результатов ("" - не запасная)
    const OSINFO *os_info;
    const WCHAR  *only;         // список /only или NULL - все разделы
    BOOL          no_network;   // /nonet - без сетевых связей
    BOOL          ping;         // /ping - опрос подключённых подсетей (сетевые связи)
} COLLECT_OPTIONS;

// Общие условия разделов одного сбора
typedef struct
{
    const COLLECT_OPTIONS *options;
    WCHAR                  computer_name[OS_COMPUTER_NAME_CCH];    // как %COMPUTERNAME%
    WCHAR                  ip_address[OS_IPV4_TEXT_CCH];           // основной IPv4 или OS_NO_IP_TEXT
    OUT_FILE               commands;                               // _команды_сбора.txt
} COLLECT_CONTEXT;

typedef BOOL (*COLLECT_RUN)(COLLECT_CONTEXT *context);

typedef struct
{
    const WCHAR        *id;         // имя для /only
    const WCHAR        *title;      // для журнала, файла команд и /list
    const WCHAR *const *outputs;    // что создаёт раздел: пути в папке результатов (файлы и папки; "*..." - маска
                                    // файлов в корне), в конце NULL; перед сбором удаляются
    COLLECT_RUN         run;        // NULL - ещё не перенесён из collect_audit.cmd
} COLLECT_SECTION;

extern const COLLECT_SECTION g_collect_sections[];
extern const int g_collect_section_count;

// Список разделов (/list).
void collect_list(void);

// Есть ли раздел с именем name (длиной length, без учёта регистра) - для проверки /only.
BOOL collect_section_exists(const WCHAR *name, size_t length);

// Выбран ли при этих ключах хоть один раздел.
BOOL collect_any_selected(const COLLECT_OPTIONS *options);

// Весь сбор: прежние результаты выбранных разделов удаляются, разделы выполняются по порядку,
// в журнал - ход сбора, в конце - строка "Finished" (по ней make_md.ps1 считает сбор завершённым).
// Журнал должен быть открыт (log_open). Возвращает число разделов, которые не удались.
int collect_run(const COLLECT_OPTIONS *options);

// Помощники разделов. relative_path - путь внутри папки результатов.

// Полный путь файла результата.
void collect_full_path(const COLLECT_CONTEXT *context, const WCHAR *relative_path, WCHAR *path, int path_cch);

// Создать папку результата (со всеми уровнями внутри папки результатов). FALSE - не удалось (в журнале).
BOOL collect_create_dir(const COLLECT_CONTEXT *context, const WCHAR *relative_dir);

// Открыть файл результата для дописывания (папки создаются). FALSE - не удалось (в журнале).
BOOL collect_open_output(const COLLECT_CONTEXT *context, const WCHAR *relative_path, OUT_FILE *file);

// Блок "[title]" и вывод программы в файл результата, команда - в файл команд, итог - в журнал.
// program - имя в System32 или полный путь. FALSE - программа не запустилась или не завершилась вовремя
// (программы нет в системе - TRUE: в файле и журнале об этом сказано).
BOOL collect_command(COLLECT_CONTEXT *context, OUT_FILE *file, const WCHAR *relative_path, const WCHAR *title,
                     const WCHAR *program, const WCHAR *arguments);

// Запись в файл команд: команда и куда записан её вывод (relative_target - путь в папке результатов, NULL -
// вывод не сохраняется). Действия функциями WinAPI - равнозначной командой с пометкой "(WinAPI)".
void collect_note_command(COLLECT_CONTEXT *context, const WCHAR *command, const WCHAR *relative_target);

#endif
