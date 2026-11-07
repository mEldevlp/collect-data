// Разделы сбора данных, по файлу sect_*.c на раздел - то, что делали разделы :collect_* в collect_audit.cmd.
#ifndef SCREENS_SECTIONS_H
#define SCREENS_SECTIONS_H

#include "collect.h"

// Папки и файлы результатов - как у collect_audit.cmd: их ищут make_md.ps1 и make_report_ib.cmd
#define SECT_GENERAL_DIR    L"00_общие_сведения"
#define SECT_SYSINFO_FILE   SECT_GENERAL_DIR L"\\00_сведения_о_системе.txt"
#define SECT_SOFTWARE_DIR   L"01_установленное_ПО"
#define SECT_TIME_DIR       L"02_синхронизация_времени"
#define SECT_AUDIT_DIR      L"03_политика_аудита"
#define SECT_ACCOUNTS_DIR   L"04_политики_учетных_записей"
#define SECT_USERS_DIR      L"05_учетные_записи_и_группы"
#define SECT_SERVICES_DIR   L"06_системные_службы"
#define SECT_REMOTE_DIR     L"07_удаленное_администрирование"
#define SECT_LOGS_DIR       L"08_файлы_log"
#define SECT_LOGS_LIST_FILE SECT_LOGS_DIR L"\\08_список_файлов_log.txt"
#define SECT_NETWORK_SUFFIX L"_сетевые_связи.txt"     // файл сетевых связей - <ИМЯ_ПК>_<IP>_сетевые_связи.txt

// 00_общие_сведения\00_сведения_о_системе.txt: имя, IP, пользователь, права, ver, systeminfo, ipconfig /all
// (блоки WMI Win32_OperatingSystem и Win32_ComputerSystem - этап 7, часть 2)
BOOL sect_sysinfo(COLLECT_CONTEXT *context);

// 08_файлы_log: список и копии файлов *.log из %WINDIR% и %WINDIR%\Inf
BOOL sect_logs(COLLECT_CONTEXT *context);

#endif
