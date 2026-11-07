// Сведения об ОС, правах и рабочем столе.
#ifndef SCREENS_OSINFO_H
#define SCREENS_OSINFO_H

#include <windows.h>

// Старшие номера версий Windows
#define OS_MAJOR_VISTA 6        // Vista, 7, 8, 8.1 (6.0-6.3); меньше - XP и Server 2003 (5.x)
#define OS_MAJOR_WIN10 10       // Windows 10 и 11

// Windows 10 20H2: с этой сборки панель "Система" открывает Параметры (там же и Windows 11)
#define OS_BUILD_WIN10_20H2 19042

// Длина строки пакета обновления - как у OSVERSIONINFOEXW.szCSDVersion
#define OS_SERVICE_PACK_CCH 128

typedef struct
{
    DWORD  major, minor, build;
    WORD   service_pack_major;
    WCHAR  service_pack[OS_SERVICE_PACK_CCH];   // "Service Pack 2"
    BOOL   is_server;
    BOOL   is_64bit_os;     // ОС 64-битная
    BOOL   is_wow64;        // мы - 32-битный процесс на 64-битной ОС
    BOOL   is_admin;        // токен в группе Администраторы (на 6.x+ без UAC-фильтра)
    LANGID ui_language;     // язык интерфейса пользователя
} OSINFO;

void os_get(OSINFO *os_info);
void os_log(const OSINFO *os_info);

// Включить DPI-awareness (на XP - ничего не делает). Вызывать до любой работы с окнами.
void os_set_dpi_aware(void);

// Можно ли снимать экран: станция WinSta0, входной стол Default.
// FALSE - причина в reason.
BOOL os_can_capture(WCHAR *reason, int reason_cch);

#define OS_IPV4_TEXT_CCH 16     // "255.255.255.255" и завершающий ноль

// Основной IPv4-адрес текстом (как в collect_audit.cmd): адрес интерфейса первого маршрута по умолчанию
// (первая такая строка "route print"), а если такого маршрута нет - первый адрес компьютера; 0.0.0.0
// и 127.x.x.x не считаются. FALSE - адреса нет.
BOOL os_primary_ipv4(WCHAR *text, int text_cch);

#endif
