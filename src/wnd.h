// Работа с окнами: запуск, поиск новых окон, раскладка, передний план, закрытие,
// подсказка с датой у часов.
#ifndef SCREENS_WND_H
#define SCREENS_WND_H

#include <windows.h>

// Класс стандартного диалога Windows (MessageBox, окна свойств, "Сохранить?")
#define WND_DIALOG_CLASS L"#32770"

// Снимок списка окон

// Видимых окон верхнего уровня в системе обычно десятки, редко сотни - берём с запасом
#define WND_SET_MAX 1024

typedef struct
{
    int  count;
    HWND list[WND_SET_MAX];
} WND_SET;

// Запомнить все видимые окна верхнего уровня.
void wnd_snapshot(WND_SET *set);
BOOL wnd_in_set(const WND_SET *set, HWND window);

// Заголовок чужого окна без риска зависнуть (WM_GETTEXT с таймаутом).
void wnd_title(HWND window, WCHAR *title, int title_cch);

// Строка "класс \"заголовок\" PID n, x,y-x,y (ШxВ)" для журнала.
void wnd_describe(HWND window, WCHAR *description, int description_cch);

// Запуск и ожидание

// Запустить командную строку и дождаться НОВОГО видимого окна верхнего уровня.
// classes - список допустимых классов (завершается NULL) или NULL - любое окно приложения
// (с заголовком, не меньше 150x100). Окно запоминается для закрытия.
// log_name - название для журнала. Возвращает окно или NULL.
HWND wnd_open(const WCHAR *command_line, const WCHAR *const *classes, DWORD timeout_ms, const WCHAR *log_name);

// То же с рабочей папкой work_dir (NULL - текущая), флагами CreateProcess (CREATE_NEW_CONSOLE для
// cmd: иначе он сядет в нашу консоль), своим STARTUPINFO (NULL - по умолчанию; размер буфера
// консоли и т. п.) и PID запущенного процесса в process_id_out (может быть NULL).
HWND wnd_open_ex(const WCHAR *command_line, const WCHAR *work_dir, DWORD create_flags,
                 const STARTUPINFOW *startup_info_in, const WCHAR *const *classes, DWORD timeout_ms,
                 const WCHAR *log_name, DWORD *process_id_out);

// Дождаться нового окна без запуска (например, диалога, открытого из окна).
// process_id - 0 или процесс, которому должно принадлежать окно.
HWND wnd_wait_new(const WND_SET *before, DWORD process_id, const WCHAR *const *classes, DWORD timeout_ms);

// Дождаться, пока окно "успокоится": картинка не меняется stable_ms и процесс окна
// почти не тратит процессор и не читает диск. FALSE - не дождались за timeout_ms.
BOOL wnd_wait_ready(HWND window, DWORD stable_ms, DWORD timeout_ms);

// Раскладка и передний план

// Рабочая область основного монитора (экран без панели задач).
void wnd_work_area(RECT *area);

// Прямоугольник окна на экране, обрезанный по виртуальному экрану (все мониторы).
void wnd_screen_rect(HWND window, RECT *rect);

// Целевое окно слева, окно сведений о системе (может быть NULL) - справа.
// Размеры - от рабочей области основного монитора. target_rect (может быть NULL) - какой
// прямоугольник назначен целевому окну (окно может его не принять: консоль, диалог).
void wnd_layout(HWND target, HWND sysinfo, RECT *target_rect);

// Поставить окна поверх всех (по порядку: последнее - самое верхнее) и сделать
// последнее активным. NULL в списке пропускаются.
void wnd_raise(HWND *windows, int count);

// Снять "поверх всех".
void wnd_unraise(HWND *windows, int count);

// Закрытие

// Запомнить окно для закрытия (wnd_open делает это сам). process - наш процесс,
// которому принадлежит окно (можно завершить), или NULL.
void wnd_track(HWND window, HANDLE process, const WCHAR *log_name);

// Действие после закрытия запомненного окна (и выхода его процесса, если он наш): например, вернуть
// файл, который программа переписала при выходе. Вызывается и при аварийной уборке; при /keep окно
// не закрывается - и действие не выполняется.
typedef void (*WND_CLOSE_ACTION)(void *argument);
void wnd_on_close(HWND window, WND_CLOSE_ACTION action, void *argument);

// Закрыть одно запомненное окно: WM_CLOSE, ответ "Нет" на вопрос о сохранении,
// через 5 с - TerminateProcess, если процесс наш.
void wnd_close(HWND window);

// Закрыть все запомненные окна в обратном порядке. keep - только снять "поверх всех".
void wnd_close_all(BOOL keep);

// Часы

// Навести курсор на часы и дождаться подсказки (до timeout_ms).
// TRUE - подсказка видна. Положение курсора запоминается.
BOOL wnd_clock_tip_show(DWORD timeout_ms);

// Спрятать "шарики" уведомлений у трея (SW_HIDE). Возвращает, сколько спрятано.
int wnd_hide_tray_balloons(void);

// Вернуть курсор на место (если его двигали).
void wnd_clock_tip_hide(void);

// Аварийная уборка (сторожевой таймер, Ctrl+C): поднять флаг прерывания, закрыть окна,
// вернуть курсор.
void wnd_emergency_cleanup(void);

// Идёт аварийное завершение: длинные шаги основного потока сразу выходят.
BOOL wnd_aborting(void);

#endif
