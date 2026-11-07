// Окно командной строки: команды вводятся "с клавиатуры", чтобы в окне было видно
// "приглашение>команда" и вывод - как при ручной работе.
//
// С чужой консолью работает отдельный процесс - копия screens.exe с ключом /conhelper:
// основной процесс сам консольный (его консоль - окно collect_audit.cmd) и не может
// прицепиться к другой консоли, не потеряв свою. Помощник запускается без консоли
// (DETACHED_PROCESS), делает AttachConsole к cmd, подгоняет размер окна, вводит команды
// через WriteConsoleInput, ждёт выполнения каждой и записывает текст консоли в файл,
// который основной процесс переносит в журнал.
#ifndef SCREENS_CONSOLE_H
#define SCREENS_CONSOLE_H

#include <windows.h>

// argv[1] начинается с /conhelper: - это запуск помощника.
BOOL con_is_helper(int argc, WCHAR **argv);

// Точка входа помощника; возвращает код выхода.
int con_helper_main(int argc, WCHAR **argv);

// Размер новой консоли при запуске (STARTF_USECOUNTCHARS + STARTF_USESIZE в startup_info), чтобы
// окно width_px x height_px было без горизонтальной прокрутки. Шрифт и рамка - как у нашей
// собственной консоли (окно collect_audit.cmd - тот же cmd с теми же настройками). Ширину надо
// задать при запуске: на XP/7 консоль при сужении буфера не переносит, а обрезает уже выведенные
// строки (шапка Win7 становилась "All rights reserv"). FALSE - своей консоли нет или она
// свёрнута (startup_info не меняется).
BOOL con_startup_size(int width_px, int height_px, STARTUPINFOW *startup_info);

// Ввести команды в консоль процесса process_id (cmd.exe), дождаться выполнения каждой.
// width_px, height_px - нужный размер окна консоли в пикселях (0 - не менять): число столбцов
// и строк подбирается под него, горизонтальной прокрутки нет.
// fit_content - после выполнения уменьшить высоту окна по содержимому (освободить место ниже).
// Текст консоли пишется в журнал. TRUE - все команды выполнены.
BOOL con_type_commands(DWORD process_id, int width_px, int height_px, BOOL fit_content,
                       const WCHAR *const *commands, int command_count, DWORD command_timeout_ms);

#endif
