#include "capture.h"
#include "common.h"
#include "log.h"
#include <string.h>

// Флаг BitBlt из wingdi.h (Windows 2000+): копировать вместе со слоистыми (layered) окнами.
#ifndef CAPTUREBLT
#define CAPTUREBLT 0x40000000
#endif

#define DIB_BITS_PER_PIXEL 32   // DIB-секция 32 бит на пиксель: строки без выравнивания, пиксель = DWORD
#define DIB_PLANES         1    // BITMAPINFOHEADER.biPlanes: по документации всегда 1

// stb_image_write: без stdio (пишем сами через CreateFileW, чтобы были Unicode-пути)
#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION

// Предупреждения MSVC, которые выдаёт сторонний stb_image_write.h на уровне /W4
// (и WARN_DEPRECATED из common.h - sprintf и другие функции CRT, которые MSVC считает небезопасными)
#define WARN_CONVERSION_DATA_LOSS     4244  // C4244: преобразование типа с возможной потерей данных
#define WARN_SIGNED_UNSIGNED_MISMATCH 4245  // C4245: преобразование знакового значения в беззнаковое
#define WARN_SIZE_T_DATA_LOSS         4267  // C4267: size_t в тип меньшего размера, возможна потеря данных
#define WARN_HIDES_LOCAL              4456  // C4456: объявление скрывает предыдущее локальное объявление
#define WARN_HIDES_PARAMETER          4457  // C4457: объявление скрывает параметр функции

#pragma warning(push, 0)
#pragma warning(disable: WARN_CONVERSION_DATA_LOSS WARN_SIGNED_UNSIGNED_MISMATCH WARN_SIZE_T_DATA_LOSS \
                         WARN_HIDES_LOCAL WARN_HIDES_PARAMETER WARN_DEPRECATED)
#include "../third_party/stb_image_write.h"
#pragma warning(pop)

// Запись PNG: файл и первая ошибка WriteFile (дальнейшие куски после ошибки не пишутся).
typedef struct
{
    HANDLE file;
    BOOL   failed;
    DWORD  error_code;
} PNG_WRITER;

// GDI-ресурсы снимка: контексты устройства (DC) экрана и памяти и DIB-секция, выбранная
// в контекст памяти. Пиксели DIB-секции доступны напрямую через bits.
typedef struct
{
    HDC     screen_dc;      // контекст устройства всего экрана (GetDC(NULL))
    HDC     memory_dc;      // контекст в памяти, совместимый с экраном: сюда копируется картинка
    HBITMAP bitmap;         // DIB-секция 32 бит на пиксель, строки сверху вниз
    HGDIOBJ old_bitmap;     // что было выбрано в memory_dc до bitmap (вернуть перед удалением)
    DWORD  *bits;           // пиксели bitmap: width * height значений 0xAARRGGBB
} GDI_SURFACE;

static void png_write_callback(void *context, void *data, int size)
{
    PNG_WRITER *writer = (PNG_WRITER *)context;
    DWORD written = 0;

    if (writer->failed || size <= 0) return;

    if (!WriteFile(writer->file, data, (DWORD)size, &written, NULL) || written != (DWORD)size)
    {
        writer->failed = TRUE;
        writer->error_code = GetLastError();
    }
}

// Текст ошибки "что делали: описание ошибки WinAPI"; error_code = 0 - только "что делали".
// error_text = NULL - текст никому не нужен.
static void set_error_text(WCHAR *error_text, int error_cch, const WCHAR *what, DWORD error_code)
{
    WCHAR system_text[TEXT_BUFFER_CCH];

    if (error_text == NULL) return;

    if (error_code == 0)
    {
        format_text(error_text, error_cch, L"%s", what);
        return;
    }

    format_text(error_text, error_cch, L"%s: %s", what, log_errtext(error_code, system_text, TEXT_BUFFER_CCH));
}

static void surface_destroy(GDI_SURFACE *surface)
{
    if (surface->old_bitmap) SelectObject(surface->memory_dc, surface->old_bitmap);
    if (surface->bitmap) DeleteObject(surface->bitmap);
    if (surface->memory_dc) DeleteDC(surface->memory_dc);
    if (surface->screen_dc) ReleaseDC(NULL, surface->screen_dc);

    ZeroMemory(surface, sizeof(*surface));
}

// Создать поверхность width x height. FALSE - ошибка (текст в error_text, если он не NULL);
// всё, что успели создать, освобождается.
static BOOL surface_create(GDI_SURFACE *surface, int width, int height, WCHAR *error_text, int error_cch)
{
    BITMAPINFO bitmap_info;
    void *bits = NULL;

    ZeroMemory(surface, sizeof(*surface));

    surface->screen_dc = GetDC(NULL);

    if (!surface->screen_dc)
    {
        set_error_text(error_text, error_cch, L"GetDC(экран)", GetLastError());
        return FALSE;
    }

    surface->memory_dc = CreateCompatibleDC(surface->screen_dc);

    if (!surface->memory_dc)
    {
        set_error_text(error_text, error_cch, L"CreateCompatibleDC", GetLastError());
        surface_destroy(surface);
        return FALSE;
    }

    ZeroMemory(&bitmap_info, sizeof(bitmap_info));
    bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap_info.bmiHeader.biWidth = width;
    bitmap_info.bmiHeader.biHeight = -height;     // отрицательная высота - строки сверху вниз
    bitmap_info.bmiHeader.biPlanes = DIB_PLANES;
    bitmap_info.bmiHeader.biBitCount = DIB_BITS_PER_PIXEL;
    bitmap_info.bmiHeader.biCompression = BI_RGB;

    surface->bitmap = CreateDIBSection(surface->screen_dc, &bitmap_info, DIB_RGB_COLORS, &bits, NULL, 0);

    if (!surface->bitmap || !bits)
    {
        set_error_text(error_text, error_cch, L"CreateDIBSection", GetLastError());
        surface_destroy(surface);
        return FALSE;
    }

    surface->bits = (DWORD *)bits;
    surface->old_bitmap = SelectObject(surface->memory_dc, surface->bitmap);

    return TRUE;
}

// Скопировать на поверхность область экрана (left, top, width x height). Сначала с CAPTUREBLT,
// чтобы попали слоистые окна (на XP так рисуется подсказка у часов); не вышло - без него.
static BOOL copy_screen_with_layered(const GDI_SURFACE *surface, int left, int top, int width, int height,
                                     WCHAR *error_text, int error_cch)
{
    if (BitBlt(surface->memory_dc, 0, 0, width, height, surface->screen_dc, left, top, SRCCOPY | CAPTUREBLT))
    {
        return TRUE;
    }

    DWORD layered_error = GetLastError();

    if (!BitBlt(surface->memory_dc, 0, 0, width, height, surface->screen_dc, left, top, SRCCOPY))
    {
        set_error_text(error_text, error_cch, L"BitBlt", GetLastError());
        return FALSE;
    }

    log_msg(L"Внимание: BitBlt с CAPTUREBLT не удался (%lu), снято без него", layered_error);

    return TRUE;
}

// Пиксели 0xAARRGGBB -> байты R, G, B подряд (PNG без альфа-канала). rgb должен быть пустым.
static BOOL pixels_to_rgb(const DWORD *pixels, size_t pixel_count, BUFFER *rgb)
{
    if (buffer_init_array(rgb, pixel_count, BYTES_PER_PIXEL_RGB) != BUFFER_OK) return FALSE;

    BYTE *target = rgb->data;

    for (size_t i = 0; i < pixel_count; i++)
    {
        target[0] = (BYTE)(pixels[i] >> PIXEL_RED_SHIFT);
        target[1] = (BYTE)(pixels[i] >> PIXEL_GREEN_SHIFT);
        target[2] = (BYTE)pixels[i];

        target += BYTES_PER_PIXEL_RGB;
    }

    return TRUE;
}

// Записать RGB-картинку в новый файл path. FALSE - ошибка: файл удалён, текст в error_text
// (если он не NULL).
static BOOL write_png_file(const WCHAR *path, const BYTE *rgb, int width, int height, WCHAR *error_text,
                           int error_cch)
{
    PNG_WRITER writer;

    writer.file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

    if (writer.file == INVALID_HANDLE_VALUE)
    {
        set_error_text(error_text, error_cch, L"не удалось создать файл", GetLastError());
        return FALSE;
    }

    writer.failed = FALSE;
    writer.error_code = 0;

    int encoded = stbi_write_png_to_func(png_write_callback, &writer, width, height, BYTES_PER_PIXEL_RGB, rgb,
                                         width * BYTES_PER_PIXEL_RGB);

    CloseHandle(writer.file);

    if (!encoded)
    {
        DeleteFileW(path);
        set_error_text(error_text, error_cch, L"ошибка кодирования PNG", 0);
        return FALSE;
    }

    if (writer.failed)
    {
        DeleteFileW(path);
        set_error_text(error_text, error_cch, L"ошибка записи файла", writer.error_code);
        return FALSE;
    }

    return TRUE;
}

// Записать PNG во временный файл и переименовать: недописанный PNG не останется под целевым именем.
static BOOL write_png_replace(const WCHAR *path, const BYTE *rgb, int width, int height, WCHAR *error_text,
                              int error_cch)
{
    WCHAR temp_path[PATH_BUFFER_CCH];

    format_text(temp_path, PATH_BUFFER_CCH, L"%s.tmp", path);

    if (!write_png_file(temp_path, rgb, width, height, error_text, error_cch)) return FALSE;

    if (!MoveFileExW(temp_path, path, MOVEFILE_REPLACE_EXISTING))
    {
        DWORD error_code = GetLastError();

        DeleteFileW(temp_path);
        set_error_text(error_text, error_cch, L"не удалось переименовать временный файл", error_code);
        return FALSE;
    }

    return TRUE;
}

// Область снимка в экранных координатах: весь виртуальный экран или основной монитор.
static void screen_area(BOOL all_monitors, RECT *area)
{
    if (!all_monitors)
    {
        SetRect(area, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
        return;
    }

    int left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int top = GetSystemMetrics(SM_YVIRTUALSCREEN);

    SetRect(area, left, top, left + GetSystemMetrics(SM_CXVIRTUALSCREEN), top + GetSystemMetrics(SM_CYVIRTUALSCREEN));
}

BOOL cap_screen_png(const WCHAR *path, BOOL all_monitors, WCHAR *error_text, int error_cch)
{
    RECT area;
    GDI_SURFACE surface;
    BUFFER rgb = { 0 };
    BOOL ok = FALSE;

    error_text[0] = 0;
    screen_area(all_monitors, &area);

    int width = area.right - area.left;
    int height = area.bottom - area.top;

    if (width <= 0 || height <= 0)
    {
        set_error_text(error_text, error_cch, L"нулевой размер экрана", 0);
        return FALSE;
    }

    if (!surface_create(&surface, width, height, error_text, error_cch)) return FALSE;

    if (copy_screen_with_layered(&surface, area.left, area.top, width, height, error_text, error_cch))
    {
        GdiFlush();

        if (pixels_to_rgb(surface.bits, (size_t)width * (size_t)height, &rgb))
        {
            ok = write_png_replace(path, rgb.data, width, height, error_text, error_cch);
        }
        else
        {
            set_error_text(error_text, error_cch, L"не хватает памяти для снимка", 0);
        }
    }

    buffer_destroy(&rgb);
    surface_destroy(&surface);

    return ok;
}

// Подготовить память кадра под width x height; при том же размере память переиспользуется.
static BOOL frame_reserve(CAP_FRAME *frame, int width, int height)
{
    if (frame->pixels.data && frame->width == width && frame->height == height) return TRUE;

    cap_frame_free(frame);

    if (buffer_init_array(&frame->pixels, (size_t)width * (size_t)height, sizeof(DWORD)) != BUFFER_OK) return FALSE;

    frame->width = width;
    frame->height = height;

    return TRUE;
}

BOOL cap_frame_grab(const RECT *rect, CAP_FRAME *frame)
{
    GDI_SURFACE surface;
    BOOL ok = FALSE;
    int width = rect->right - rect->left;
    int height = rect->bottom - rect->top;

    if (width <= 0 || height <= 0) return FALSE;
    if (!frame_reserve(frame, width, height)) return FALSE;
    if (!surface_create(&surface, width, height, NULL, 0)) return FALSE;

    if (BitBlt(surface.memory_dc, 0, 0, width, height, surface.screen_dc, rect->left, rect->top, SRCCOPY))
    {
        GdiFlush();
        memcpy(frame->pixels.data, surface.bits, frame->pixels.size);
        ok = TRUE;
    }

    surface_destroy(&surface);

    return ok;
}

BOOL cap_frame_copy(CAP_FRAME *destination, const CAP_FRAME *source)
{
    if (!source->pixels.data) return FALSE;
    if (!frame_reserve(destination, source->width, source->height)) return FALSE;

    memcpy(destination->pixels.data, source->pixels.data, source->pixels.size);

    return TRUE;
}

void cap_frame_swap(CAP_FRAME *first, CAP_FRAME *second)
{
    CAP_FRAME saved = *first;

    *first = *second;
    *second = saved;
}

void cap_frame_free(CAP_FRAME *frame)
{
    buffer_destroy(&frame->pixels);

    frame->width = 0;
    frame->height = 0;
}

BOOL cap_frame_png(const CAP_FRAME *frame, const WCHAR *path)
{
    BUFFER rgb = { 0 };

    if (!frame->pixels.data) return FALSE;
    if (!pixels_to_rgb(frame->pixels.data, (size_t)frame->width * (size_t)frame->height, &rgb)) return FALSE;

    BOOL ok = write_png_file(path, rgb.data, frame->width, frame->height, NULL, 0);

    buffer_destroy(&rgb);

    return ok;
}

long cap_frame_diff(const CAP_FRAME *first, const CAP_FRAME *second)
{
    if (!first->pixels.data || !second->pixels.data) return CAP_FRAME_DIFF_INVALID;
    if (first->width != second->width || first->height != second->height) return CAP_FRAME_DIFF_INVALID;

    const DWORD *first_pixels = first->pixels.data;
    const DWORD *second_pixels = second->pixels.data;
    size_t pixel_count = (size_t)first->width * (size_t)first->height;
    long different = 0;

    for (size_t i = 0; i < pixel_count; i++)
    {
        if ((first_pixels[i] ^ second_pixels[i]) & PIXEL_RGB_MASK) different++;
    }

    return different;
}
