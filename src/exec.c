#include "exec.h"
#include "common.h"
#include "text.h"
#include <string.h>

// Временный файл вывода: %TEMP%\scrXXXX.tmp, удаляется системой при закрытии последнего описателя
#define OUTPUT_FILE_PREFIX L"scr"

// Сколько ждать выхода процесса после TerminateJobObject / TerminateProcess
#define TERMINATE_WAIT_MS 5000

// Ответ "нет" на вопрос net "запустить службу?"
static const char ANSWER_NO_TEXT[] = "N\r\n";

static CRITICAL_SECTION g_exec_lock;
static BOOL             g_exec_lock_ready = FALSE;
static HANDLE           g_running_job = NULL;       // job-объект выполняющейся программы
static HANDLE           g_running_process = NULL;   // сам процесс (если в job-объект поместить не удалось)

static SECURITY_ATTRIBUTES inheritable_attributes(void)
{
    SECURITY_ATTRIBUTES attributes;

    ZeroMemory(&attributes, sizeof(attributes));
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;

    return attributes;
}

// Полный путь программы: имя без папки - в System32.
static void program_path(const WCHAR *program, WCHAR *path, int path_cch)
{
    WCHAR system_dir[MAX_PATH];

    if (wcschr(program, L'\\'))
    {
        lstrcpynW(path, program, path_cch);
        return;
    }

    UINT length = GetSystemDirectoryW(system_dir, MAX_PATH);

    if (length == 0 || length >= MAX_PATH) lstrcpyW(system_dir, L"C:\\Windows\\system32");

    format_text(path, path_cch, L"%s\\%s", system_dir, program);
}

// Командная строка для CreateProcess (путь в кавычках) и для журнала (кавычки - только если в пути пробел).
static void build_command_lines(const WCHAR *path, const WCHAR *arguments, WCHAR *command, WCHAR *display)
{
    const WCHAR *separator = (arguments && arguments[0]) ? L" " : L"";
    const WCHAR *tail = arguments ? arguments : L"";
    const WCHAR *quote = wcschr(path, L' ') ? L"\"" : L"";

    format_text(command, EXEC_COMMAND_LINE_CCH, L"\"%s\"%s%s", path, separator, tail);
    format_text(display, EXEC_COMMAND_LINE_CCH, L"%s%s%s%s%s", quote, path, quote, separator, tail);
}

// Временный файл для stdout и stderr: наследуемый описатель, удаляется при закрытии.
static HANDLE open_output_file(void)
{
    WCHAR temp_dir[MAX_PATH];
    WCHAR path[MAX_PATH];
    SECURITY_ATTRIBUTES attributes = inheritable_attributes();
    DWORD length = GetTempPathW(MAX_PATH, temp_dir);

    if (length == 0 || length >= MAX_PATH) return INVALID_HANDLE_VALUE;
    if (!GetTempFileNameW(temp_dir, OUTPUT_FILE_PREFIX, 0, path)) return INVALID_HANDLE_VALUE;

    DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    HANDLE file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, share, &attributes, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);

    if (file == INVALID_HANDLE_VALUE) DeleteFileW(path);

    return file;
}

// Вход программы: NUL или канал с ответом "N" (после ответа - конец ввода).
static HANDLE open_input(BOOL answer_no)
{
    SECURITY_ATTRIBUTES attributes = inheritable_attributes();

    if (!answer_no)
    {
        return CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes, OPEN_EXISTING, 0,
                           NULL);
    }

    HANDLE read_end = NULL;
    HANDLE write_end = NULL;
    DWORD written = 0;

    if (!CreatePipe(&read_end, &write_end, &attributes, 0)) return INVALID_HANDLE_VALUE;

    // наследуется только конец для чтения: иначе программа не дождалась бы конца ввода
    SetHandleInformation(write_end, HANDLE_FLAG_INHERIT, 0);
    WriteFile(write_end, ANSWER_NO_TEXT, (DWORD)LITERAL_LENGTH(ANSWER_NO_TEXT), &written, NULL);
    CloseHandle(write_end);

    return read_end;
}

// Job-объект: при закрытии описателя (в том числе при аварийном выходе утилиты) завершает все свои процессы.
static HANDLE create_job(void)
{
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
    HANDLE job = CreateJobObjectW(NULL, NULL);

    if (!job) return NULL;

    ZeroMemory(&limits, sizeof(limits));
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));

    return job;
}

static void ensure_exec_lock(void)
{
    if (g_exec_lock_ready) return;

    InitializeCriticalSection(&g_exec_lock);
    g_exec_lock_ready = TRUE;
}

static void set_running(HANDLE job, HANDLE process)
{
    EnterCriticalSection(&g_exec_lock);
    g_running_job = job;
    g_running_process = process;
    LeaveCriticalSection(&g_exec_lock);
}

// Запустить процесс приостановленным, поместить в job-объект и продолжить. FALSE - не запустился.
static BOOL start_process(const WCHAR *path, WCHAR *command, HANDLE input, HANDLE output, HANDLE job,
                          PROCESS_INFORMATION *process_info)
{
    STARTUPINFOW startup_info;

    ZeroMemory(&startup_info, sizeof(startup_info));
    startup_info.cb = sizeof(startup_info);
    startup_info.dwFlags = STARTF_USESTDHANDLES;
    startup_info.hStdInput = input;
    startup_info.hStdOutput = output;
    startup_info.hStdError = output;

    // консоль - наша: так программа пишет в кодовой странице консоли, как в collect_audit.cmd
    if (!CreateProcessW(path, command, NULL, NULL, TRUE, CREATE_SUSPENDED, NULL, NULL, &startup_info, process_info))
    {
        return FALSE;
    }

    // не удалось (Windows до 8 не вкладывает job-объекты, если нас самих запустили в нём) - работаем без него
    BOOL in_job = job && AssignProcessToJobObject(job, process_info->hProcess);

    set_running(in_job ? job : NULL, process_info->hProcess);
    ResumeThread(process_info->hThread);
    CloseHandle(process_info->hThread);
    process_info->hThread = NULL;

    return TRUE;
}

static void terminate_running(void)
{
    EnterCriticalSection(&g_exec_lock);

    if (g_running_job) TerminateJobObject(g_running_job, TERMINATED_EXIT_CODE);
    else if (g_running_process) TerminateProcess(g_running_process, TERMINATED_EXIT_CODE);

    LeaveCriticalSection(&g_exec_lock);
}

// Дождаться процесса; не завершился за timeout_ms - остановить (вместе с дочерними).
static void wait_process(HANDLE process, DWORD timeout_ms, EXEC_RESULT *result)
{
    if (WaitForSingleObject(process, timeout_ms) == WAIT_TIMEOUT)
    {
        result->timed_out = TRUE;
        terminate_running();
        WaitForSingleObject(process, TERMINATE_WAIT_MS);
    }

    if (!GetExitCodeProcess(process, &result->exit_code)) result->exit_code = 0;
}

// Вывод из временного файла (не больше EXEC_OUTPUT_MAX_BYTES) - в bytes. FALSE - ошибка, код в *error.
static BOOL read_output(HANDLE file, BUFFER *bytes, BOOL *truncated, DWORD *error)
{
    LARGE_INTEGER size;
    LARGE_INTEGER start;

    *truncated = FALSE;
    buffer_init(bytes, 0);

    if (!GetFileSizeEx(file, &size))
    {
        *error = GetLastError();
        return FALSE;
    }

    if (size.QuadPart > EXEC_OUTPUT_MAX_BYTES)
    {
        size.QuadPart = EXEC_OUTPUT_MAX_BYTES;
        *truncated = TRUE;
    }

    if (size.QuadPart == 0) return TRUE;

    start.QuadPart = 0;

    if (!SetFilePointerEx(file, start, NULL, FILE_BEGIN) || buffer_init(bytes, (size_t)size.QuadPart) != BUFFER_OK)
    {
        *error = GetLastError();
        return FALSE;
    }

    size_t done = 0;

    while (done < bytes->size)
    {
        DWORD read = 0;

        if (!ReadFile(file, (BYTE *)bytes->data + done, (DWORD)(bytes->size - done), &read, NULL) || read == 0) break;

        done += read;
    }

    bytes->size = done;

    return TRUE;
}

// Кодовые страницы вывода: консоли (как в collect_audit.cmd, обычно OEM - 866) и ANSI (1251: так пишут reg и
// xcopy на XP, winrm.vbs).
static void output_code_pages(UINT *primary, UINT *secondary)
{
    *primary = GetConsoleOutputCP();

    if (*primary == 0) *primary = GetOEMCP();

    *secondary = GetACP();
}

static BOOL decode_output(HANDLE file, EXEC_RESULT *result)
{
    BUFFER bytes = { 0 };
    UINT primary = 0;
    UINT secondary = 0;

    if (!read_output(file, &bytes, &result->truncated, &result->error)) return FALSE;

    output_code_pages(&primary, &secondary);

    int decoded = text_decode(bytes.data, bytes.size, primary, secondary, &result->text);

    buffer_destroy(&bytes);

    if (decoded != BUFFER_OK) result->error = ERROR_NOT_ENOUGH_MEMORY;

    return decoded == BUFFER_OK;
}

// Код последней ошибки WinAPI; 0 (функция ошибку не задала) - общий код, чтобы сбой не приняли за успех.
static DWORD failure_code(void)
{
    DWORD error = GetLastError();

    return error ? error : ERROR_GEN_FAILURE;
}

static BOOL is_not_found_error(DWORD error)
{
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

BOOL exec_run(const EXEC_REQUEST *request, EXEC_RESULT *result)
{
    WCHAR path[PATH_BUFFER_CCH];
    WCHAR command[EXEC_COMMAND_LINE_CCH];
    PROCESS_INFORMATION process_info;

    ZeroMemory(result, sizeof(*result));
    ZeroMemory(&process_info, sizeof(process_info));
    ensure_exec_lock();
    program_path(request->program, path, PATH_BUFFER_CCH);
    build_command_lines(path, request->arguments, command, result->command_line);

    // код ошибки - сразу после неудачного шага, пока его не затёрли следующие вызовы
    HANDLE input = INVALID_HANDLE_VALUE;
    HANDLE job = NULL;
    HANDLE output = open_output_file();

    if (output == INVALID_HANDLE_VALUE) result->error = failure_code();
    else input = open_input(request->answer_no);

    if (!result->error && input == INVALID_HANDLE_VALUE) result->error = failure_code();
    if (!result->error) job = create_job();

    DWORD start_tick = GetTickCount();

    if (!result->error && !start_process(path, command, input, output, job, &process_info))
    {
        result->error = failure_code();
        result->not_found = is_not_found_error(result->error);
    }

    if (!result->error)
    {
        result->started = TRUE;
        wait_process(process_info.hProcess, request->timeout_ms, result);
        set_running(NULL, NULL);
        CloseHandle(process_info.hProcess);
    }

    result->elapsed_ms = GetTickCount() - start_tick;

    // закрытие job-объекта завершает оставшиеся дочерние процессы программы
    if (job) CloseHandle(job);
    if (input != INVALID_HANDLE_VALUE) CloseHandle(input);

    BOOL read_ok = result->started && decode_output(output, result);

    if (output != INVALID_HANDLE_VALUE) CloseHandle(output);

    return read_ok && !result->timed_out;
}

void exec_free(EXEC_RESULT *result)
{
    buffer_destroy(&result->text);
}

void exec_abort(void)
{
    if (!g_exec_lock_ready) return;

    terminate_running();
}
