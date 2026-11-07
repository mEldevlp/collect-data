#include "names.h"
#include "common.h"
#include <string.h>

// Название после приведения (нижний регистр, "ё" -> "е"). Названия узлов короче; длиннее - обрезается.
#define NORMALIZED_CCH 260

#define VARIANT_SEPARATOR L" / "

// Узлы gpedit.msc
static const NAMES_NODE COMPUTER_CONFIGURATION = { { L"Конфигурация компьютера", L"Computer Configuration" } };
static const NAMES_NODE WINDOWS_SETTINGS = { { L"Конфигурация Windows", L"Windows Settings" } };
static const NAMES_NODE SECURITY_SETTINGS = { { L"Параметры безопасности", L"Security Settings" } };
static const NAMES_NODE ACCOUNT_POLICIES = { { L"Политики учетных записей", L"Account Policies" } };
static const NAMES_NODE PASSWORD_POLICY = { { L"Политика паролей", L"Password Policy" } };
static const NAMES_NODE LOCKOUT_POLICY = { { L"Политика блокировки учетной записи", L"Account Lockout Policy" } };
static const NAMES_NODE LOCAL_POLICIES = { { L"Локальные политики", L"Local Policies" } };
static const NAMES_NODE AUDIT_POLICY = { { L"Политика аудита", L"Audit Policy" } };

const NAMES_PATH g_names_audit_policy =
{
    L"Политика аудита",
    { &COMPUTER_CONFIGURATION, &WINDOWS_SETTINGS, &SECURITY_SETTINGS, &LOCAL_POLICIES, &AUDIT_POLICY, NULL }
};

const NAMES_PATH g_names_password_policy =
{
    L"Политика паролей",
    { &COMPUTER_CONFIGURATION, &WINDOWS_SETTINGS, &SECURITY_SETTINGS, &ACCOUNT_POLICIES, &PASSWORD_POLICY, NULL }
};

const NAMES_PATH g_names_lockout_policy =
{
    L"Политика блокировки учётной записи",
    { &COMPUTER_CONFIGURATION, &WINDOWS_SETTINGS, &SECURITY_SETTINGS, &ACCOUNT_POLICIES, &LOCKOUT_POLICY, NULL }
};

// Привести название к виду для сравнения: "ё" -> "е", нижний регистр.
static void normalize(const WCHAR *text, WCHAR *normalized, int normalized_cch)
{
    lstrcpynW(normalized, text, normalized_cch);

    for (WCHAR *symbol = normalized; *symbol; symbol++)
    {
        if (*symbol == L'ё') *symbol = L'е';
        else if (*symbol == L'Ё') *symbol = L'Е';
    }

    CharLowerBuffW(normalized, (DWORD)wcslen(normalized));
}

BOOL names_match(const WCHAR *text, const NAMES_NODE *node, BOOL prefix_allowed)
{
    WCHAR normalized_text[NORMALIZED_CCH];
    WCHAR normalized_variant[NORMALIZED_CCH];

    normalize(text, normalized_text, NORMALIZED_CCH);

    size_t text_length = wcslen(normalized_text);

    for (int i = 0; i < NAMES_MAX_VARIANTS && node->variants[i]; i++)
    {
        normalize(node->variants[i], normalized_variant, NORMALIZED_CCH);

        size_t variant_length = wcslen(normalized_variant);

        if (variant_length == 0 || variant_length > text_length) continue;
        if (variant_length == text_length && wcscmp(normalized_text, normalized_variant) == 0) return TRUE;
        if (prefix_allowed && wcsncmp(normalized_text, normalized_variant, variant_length) == 0) return TRUE;
    }

    return FALSE;
}

void names_describe(const NAMES_NODE *node, WCHAR *text, int text_cch)
{
    int length = 0;

    text[0] = 0;

    for (int i = 0; i < NAMES_MAX_VARIANTS && node->variants[i] && length >= 0; i++)
    {
        int added = format_text(text + length, text_cch - length, L"%s\"%s\"", i > 0 ? VARIANT_SEPARATOR : L"",
                                node->variants[i]);

        length = added < 0 ? -1 : length + added;
    }
}
