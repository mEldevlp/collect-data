// Постраничная съёмка прокручиваемого списка (колесо мыши + сравнение кадров).
// Не зависит от устройства списка: работает и с DirectUI (XP ARP, "Программы и компоненты").
#ifndef SCREENS_SCROLL_H
#define SCREENS_SCROLL_H

#include <windows.h>

// Снять страницу page из page_count (1..page_count). TRUE - снимок сделан.
typedef BOOL (*SCROLL_SHOT)(int page, int page_count, void *arg);

// Список в окне window должен быть прокручен в начало. Измеряет полную длину прокрутки,
// делит на страницы (не больше max_pages) с равным шагом (одинаковое перекрытие) и для каждой
// вызывает shot. Если прокрутки нет - одна страница.
// Возвращает число страниц, для которых shot вернул TRUE; *page_count_out - сколько было страниц.
int scroll_pages(HWND window, int max_pages, BOOL dump, SCROLL_SHOT shot, void *arg, int *page_count_out);

// Папка для диагностических кадров (/dump): при неудачном измерении сдвига туда пишутся
// _прокрутка_N_до.png / _после.png. NULL - не писать.
void scroll_set_debug_dir(const WCHAR *dir);

#endif
