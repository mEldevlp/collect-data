// Снимок экрана и запись PNG.
#ifndef SCREENS_CAPTURE_H
#define SCREENS_CAPTURE_H

#include <windows.h>
#include "buffer.h"

// cap_frame_diff: кадры нельзя сравнить (разного размера или пустые).
#define CAP_FRAME_DIFF_INVALID (-1)

// Снять экран в PNG-файл path.
// all_monitors: TRUE - весь виртуальный экран, FALSE - основной монитор.
// При ошибке FALSE и текст в error_text.
BOOL cap_screen_png(const WCHAR *path, BOOL all_monitors, WCHAR *error_text, int error_cch);

// Кадр области экрана (для ожидания, пока окно перестанет меняться, и сравнения прокрутки).
typedef struct
{
    int    width, height;
    BUFFER pixels;      // width * height значений DWORD 0xAARRGGBB, строки сверху вниз;
                        // память освобождает cap_frame_free
} CAP_FRAME;

// Снять область rect (экранные координаты) в кадр frame (память переиспользуется).
BOOL cap_frame_grab(const RECT *rect, CAP_FRAME *frame);

// Скопировать кадр source в destination (память destination переиспользуется).
BOOL cap_frame_copy(CAP_FRAME *destination, const CAP_FRAME *source);

// Обменять кадры местами (без копирования пикселей).
void cap_frame_swap(CAP_FRAME *first, CAP_FRAME *second);

void cap_frame_free(CAP_FRAME *frame);

// Сколько пикселей различается; CAP_FRAME_DIFF_INVALID - кадры разного размера или пустые.
long cap_frame_diff(const CAP_FRAME *first, const CAP_FRAME *second);

// Записать кадр в PNG (диагностика).
BOOL cap_frame_png(const CAP_FRAME *frame, const WCHAR *path);

#endif
