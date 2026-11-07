# Сверка результатов сбора данных (папка <ИМЯ_ПК>_<IP>) с эталоном - результатом collect_audit.cmd на той же ВМ.
#  - файлы сопоставляются по пути внутри папки; текстовые (*.txt, *.csv) делятся на блоки по заголовкам
#    "[...]" и "===== ... =====", как их делит make_md.ps1, блоки сравниваются по набору строк;
#  - в строках маскируются дата и время; в изменчивых блоках (-VolatileBlocks: netstat, tasklist, arp и т. п.)
#    ещё и все числа, их отличия - справочно и в итог не входят;
#  - пробелы схлопываются, пустые строки не считаются: сверяется содержание, а не выравнивание (-Exact - строго);
#  - у файлов результата проверяется формат: UTF-8 с BOM и переводы строк CRLF;
#  - -Report: make_md.ps1 по копиям обеих папок и сверка отчётов по разделам "#".
# Отчёт - <OutDir>\_сверка_с_эталоном.md (UTF-8 с BOM), в консоль - строка "=== СВЕРКА ...".
# Эталон ищется в -EtalonRoot по имени компьютера: папка "<ИМЯ_ПК>_*". Результат - папка прогона vm_test.ps1
# (имя папки утилиты - в _folder.txt) или сама папка <ИМЯ_ПК>_<IP>.
# Пример: .\compare_collect.ps1 -Actual results\audit_xpsp2_20261108_101500 -Report
param(
    [Parameter(Mandatory = $true)][string]$Actual,  # одна или несколько папок через запятую
    [string]$Etalon = '',                           # явная папка эталона (только для одной папки -Actual)
    [string]$EtalonRoot = '',                       # по умолчанию ..\..\VMs
    [string]$OutDir = '',                           # куда писать отчёт; по умолчанию - в папку -Actual
    [switch]$Report,                                # сверить и отчёты make_md.ps1
    [switch]$Exact,                                 # не схлопывать пробелы и учитывать пустые строки
    [int]$MaxExamples = 12,                         # сколько отличающихся строк показывать на блок
    [string[]]$VolatileBlocks = @(
        'netstat*', 'arp -a*', 'tasklist*', '*Win32_Process*', 'ipconfig /displaydns', 'nbtstat -c', 'nbtstat -S', 'nbtstat -r', 'nbtstat -n',
        'net session*', 'systeminfo', 'w32tm /query /status', 'w32tm /query /peers', 'Win32_OperatingSystem',
        'netsh interface ipv4 show neighbors', 'Collection', 'Finished', 'dir *', 'Addresses that answered ping',
        'ARP entries*'),
    # строки, значение в которых меняется от загрузки к загрузке (регулярные выражения): значение после
    # типа REG_* или двоеточия заменяется на <изменчиво>; найдено прогонами скрипта на одной ВМ от clean
    [string[]]$VolatileLines = @('^\s*LsaPid\s', 'DUID', '^\s*InstanceID\s+REG_', '^\s*Guid\s+REG_BINARY'),
    # разделы отчёта make_md.ps1, которые меняются от прогона к прогону (числа замаскированы, справочно):
    # сводка (число работающих служб, порты), systeminfo (время работы, память), журнал, процессы, список файлов,
    # файлы *.log (размер WindowsUpdate.log и др. растёт с каждой загрузкой)
    [string[]]$VolatileSections = @('Краткая сводка', '*systeminfo*', '*Журнал сбора*', '*Открытые порты и процессы*',
        '*Исходные файлы*', '8. Файлы журналов*')
)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $EtalonRoot) { $EtalonRoot = Join-Path $here '..\..\VMs' }
$makeMd = Join-Path $here '..\..\make_md.ps1'

try { [Text.Encoding]::RegisterProvider([Text.CodePagesEncodingProvider]::Instance) } catch { }
$script:Oem = [Text.Encoding]::GetEncoding(866)
$script:Ansi = [Text.Encoding]::GetEncoding(1251)
$script:Utf8 = New-Object Text.UTF8Encoding($false, $false)
$script:Latin1 = [Text.Encoding]::GetEncoding(28591)
$script:HighByteRx = New-Object Text.RegularExpressions.Regex('[\x80-\xFF]')
$script:Bad = [char]0xFFFD
$script:WordRx = New-Object Text.RegularExpressions.Regex('[А-я]{2,}')
$script:JunkRx = New-Object Text.RegularExpressions.Regex('[^\s!-~А-яЁё«»–—№]')
$script:WmiDateRx = New-Object Text.RegularExpressions.Regex('\b\d{14}\.\d{6}[+-]\d{3}\b')
$script:DateRx = New-Object Text.RegularExpressions.Regex('(?<![\d.])\d{1,2}[./]\d{1,2}[./](?:\d{4}|\d{2})(?![\d.])')
$script:IsoDateRx = New-Object Text.RegularExpressions.Regex('(?<!\d)\d{4}-\d{2}-\d{2}(?!\d)')
# дата словами: журнал secedit пишет "4 ноября 2026 г." (RU) и "November 4, 2026" (EN)
$script:LongDateRx = New-Object Text.RegularExpressions.Regex(
    '\b\d{1,2} (?:января|февраля|марта|апреля|мая|июня|июля|августа|сентября|октября|ноября|декабря) \d{4}(?: г\.)?|' +
    '\b(?:January|February|March|April|May|June|July|August|September|October|November|December) \d{1,2}, \d{4}\b')
# число в скобках в заголовке отчёта make_md.ps1: "6.1. Список служб (80)"
$script:CountRx = New-Object Text.RegularExpressions.Regex('\(\d+\)')
# время не берётся из адресов вида 0.0.0.0:135: перед ним не цифра, не точка и не двоеточие
$script:TimeRx = New-Object Text.RegularExpressions.Regex(
    '(?<![\d.:])\d{1,2}:\d{2}(?::\d{2}(?:[.,]\d+)?)?(?:\s?[AP]M)?(?![\d:])')
$script:DigitsRx = New-Object Text.RegularExpressions.Regex('\d+')
$script:SpaceRx = New-Object Text.RegularExpressions.Regex('\s+')
# суффикс пользовательских служб Win10 (CDPUserSvc_3da44) - номер сеанса входа, свой при каждой загрузке
# (и в отображаемом имени: "...трансляции_3da44")
$script:UserServiceRx = New-Object Text.RegularExpressions.Regex('(?<=\p{L})_(?=[0-9a-f]*\d)[0-9a-f]{4,8}\b')
# состояние службы: службы по триггеру (Win10) от прогона к прогону то запущены, то нет
$script:ServiceStateRx = New-Object Text.RegularExpressions.Regex(
    '\b(?:RUNNING|STOPPED|START_PENDING|STOP_PENDING|Running|Stopped|Start Pending|Stop Pending|' +
    'работает|остановлена|запускается|останавливается)\b')
$script:VolatileValueRx = New-Object Text.RegularExpressions.Regex('(REG_\w+|:)\s*.*$')
$script:VolatileLineRx = @($VolatileLines | ForEach-Object { New-Object Text.RegularExpressions.Regex($_) })
$script:HeaderRx = New-Object Text.RegularExpressions.Regex('^\[(.+)\]$')
# заголовок раздела сетевых связей "===== 1. General information =====": внутри текст, а не строка
# разделителя таблицы tasklist "===== ======== ====="
$script:SectionRx = New-Object Text.RegularExpressions.Regex('^={3,}\s+([^=\s].*?)\s+={3,}$')
$script:MdHeadingRx = New-Object Text.RegularExpressions.Regex('^#{1,6}\s+(.+)$')

# Служебные файлы прогона vm_test.ps1 и файлы, которые сверка не трогает.
$script:SkipPatterns = @('done.txt', 'exitcode.txt', 'started.txt', '_stdout.txt', '_folder.txt', '_prep.txt',
    '*.png', '*_отчёт.md', '_запуск_в_ВМ.txt', '_сверка_с_эталоном.md', '_отчёт_эталон.md', '_отчёт_результат.md',
    '_журнал_скриншотов.txt')
# Файлы, у которых сверяется только наличие: группа - папка или сам файл.
$script:NameOnlyGroups = @('выгрузка', '08_файлы_log\WINDIR', '08_файлы_log\WINDIR_Inf')
$script:NameOnlyFiles = @('_команды_сбора.txt', '_журнал_сбора.txt')


# ---------------------------------------------------------------------------
# Чтение файлов
# ---------------------------------------------------------------------------

# Оценка строки для выбора между 866 и 1251, как в collect_audit.cmd и make_md.ps1.
function Get-LineScore([string]$Text) {
    $n = 0
    foreach ($m in $script:WordRx.Matches($Text)) { $n += $m.Length }
    return $n - $script:JunkRx.Matches($Text).Count
}

# Текст файла, его кодировка и переводы строк. Без BOM: UTF-8, а если он не читается - построчно 866 или 1251.
function Read-TextFile([string]$Path) {
    $bytes = [IO.File]::ReadAllBytes($Path)
    $encoding = ''
    if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) {
        $text = $script:Utf8.GetString($bytes, 3, $bytes.Length - 3)
        $encoding = 'UTF-8 BOM'
    }
    elseif ($bytes.Length -ge 2 -and $bytes[0] -eq 0xFF -and $bytes[1] -eq 0xFE) {
        $text = [Text.Encoding]::Unicode.GetString($bytes, 2, $bytes.Length - 2)
        $encoding = 'UTF-16'
    }
    else {
        $text = $script:Utf8.GetString($bytes)
        # байты >= 0x80 ищутся регулярным выражением по тексту в Latin-1: цикл по байтам в PowerShell медленный
        $ascii = -not $script:HighByteRx.IsMatch($script:Latin1.GetString($bytes))
        if ($ascii) { $encoding = 'ASCII без BOM' }
        elseif ($text.IndexOf($script:Bad) -lt 0) { $encoding = 'UTF-8 без BOM' }
        else {
            $encoding = '866/1251'
            $sb = New-Object Text.StringBuilder
            $start = 0
            while ($start -lt $bytes.Length) {
                $end = [Array]::IndexOf($bytes, [byte]10, $start)
                if ($end -lt 0) { $end = $bytes.Length } else { $end++ }
                $line = $script:Utf8.GetString($bytes, $start, $end - $start)
                if ($line.IndexOf($script:Bad) -ge 0) {
                    $line = $script:Oem.GetString($bytes, $start, $end - $start)
                    $alt = $script:Ansi.GetString($bytes, $start, $end - $start)
                    if ((Get-LineScore $alt) -gt (Get-LineScore $line)) { $line = $alt }
                }
                [void]$sb.Append($line)
                $start = $end
            }
            $text = $sb.ToString()
        }
    }
    $lineFeeds = ([regex]::Matches($text, "`n")).Count
    $crLf = ([regex]::Matches($text, "`r`n")).Count
    $lines = New-Object 'System.Collections.Generic.List[string]'
    $lines.AddRange([string[]]($text -split "`r?`n"))
    if ($lines.Count -gt 0 -and $lines[$lines.Count - 1] -eq '') { $lines.RemoveAt($lines.Count - 1) }
    return [pscustomobject]@{ Lines = $lines.ToArray(); Encoding = $encoding; LineFeeds = $lineFeeds; CrLf = $crLf }
}

function Get-FormatText($File) {
    $ends = 'CRLF'
    if ($File.LineFeeds -eq 0) { $ends = 'без переводов строк' }
    elseif ($File.CrLf -eq 0) { $ends = 'LF' }
    elseif ($File.CrLf -lt $File.LineFeeds) { $ends = 'CRLF и LF' }
    $text = $File.Encoding + ', ' + $ends
    if ($File.Encoding -ne 'UTF-8 BOM' -or ($ends -ne 'CRLF' -and $ends -ne 'без переводов строк')) { $text += ' (!)' }
    return $text
}


# ---------------------------------------------------------------------------
# Блоки и сравнение строк
# ---------------------------------------------------------------------------

function ConvertTo-Normal([string]$Line, [bool]$Volatile) {
    $t = $script:WmiDateRx.Replace($Line, '<дата>')
    $t = $script:LongDateRx.Replace($t, '<дата>')
    $t = $script:DateRx.Replace($t, '<дата>')
    $t = $script:IsoDateRx.Replace($t, '<дата>')
    $t = $script:TimeRx.Replace($t, '<время>')
    $t = $script:UserServiceRx.Replace($t, '_<сеанс>')
    $t = $script:ServiceStateRx.Replace($t, '<состояние>')
    foreach ($rx in $script:VolatileLineRx) {
        if ($rx.IsMatch($t)) { $t = $script:VolatileValueRx.Replace($t, '$1 <изменчиво>'); break }
    }
    if ($Volatile) { $t = $script:DigitsRx.Replace($t, 'N') }
    if ($Exact) { return $t.TrimEnd() }
    return $script:SpaceRx.Replace($t, ' ').Trim()
}

function Test-Volatile([string]$Title, [bool]$Markdown) {
    $patterns = $VolatileBlocks
    if ($Markdown) { $patterns = $VolatileSections }
    foreach ($p in $patterns) { if ($Title -like $p) { return $true } }
    return $false
}

# Делит строки на блоки. Заголовок блока - "[...]" (кроме строк sc "[SC] ..."), "===== ... =====" или,
# для Markdown, строка "# ...". Ключ блока - заголовок без даты и времени и номер повтора.
function Get-Blocks([string[]]$Lines, [bool]$Markdown) {
    $blocks = New-Object 'System.Collections.Generic.List[object]'
    $seen = @{}
    $current = [pscustomobject]@{ Key = '(начало файла)'; Title = '(начало файла)'; Volatile = $false
        Lines = (New-Object 'System.Collections.Generic.List[string]'); Raw = (New-Object 'System.Collections.Generic.List[string]') }
    $blocks.Add($current)
    foreach ($line in $Lines) {
        $trimmed = $line.TrimEnd()
        $title = $null
        if ($Markdown) {
            $m = $script:MdHeadingRx.Match($trimmed)
            if ($m.Success) { $title = $m.Groups[1].Value }
        }
        else {
            $m = $script:HeaderRx.Match($trimmed)
            if ($m.Success -and -not $trimmed.StartsWith('[SC]')) { $title = $m.Groups[1].Value }
            else {
                $m = $script:SectionRx.Match($trimmed)
                if ($m.Success) { $title = $m.Groups[1].Value }
            }
        }
        if ($null -ne $title) {
            $normalTitle = ConvertTo-Normal $title $false
            if ($Markdown) { $normalTitle = $script:CountRx.Replace($normalTitle, '(N)') }
            $key = $normalTitle
            if ($seen.ContainsKey($normalTitle)) { $seen[$normalTitle]++; $key = $normalTitle + ' #' + $seen[$normalTitle] }
            else { $seen[$normalTitle] = 1 }
            $current = [pscustomobject]@{ Key = $key; Title = $title.Trim(); Volatile = (Test-Volatile $normalTitle $Markdown)
                Lines = (New-Object 'System.Collections.Generic.List[string]'); Raw = (New-Object 'System.Collections.Generic.List[string]') }
            $blocks.Add($current)
            continue
        }
        $normal = ConvertTo-Normal $line $current.Volatile
        if (-not $Exact -and $normal -eq '') { continue }
        $current.Lines.Add($normal)
        $current.Raw.Add($line.TrimEnd())
    }
    return , $blocks
}

# Сравнение двух наборов строк без учёта порядка: строки, которых в другом наборе нет (с учётом повторов),
# в исходном порядке. Ordered - совпали ли строки и по порядку.
function Compare-LineSets($Left, $Right) {
    $count = New-Object 'System.Collections.Generic.Dictionary[string,int]'
    foreach ($l in $Left.Lines) { if ($count.ContainsKey($l)) { $count[$l]++ } else { $count[$l] = 1 } }
    foreach ($l in $Right.Lines) { if ($count.ContainsKey($l)) { $count[$l]-- } else { $count[$l] = -1 } }
    $removed = New-Object 'System.Collections.Generic.List[string]'
    $added = New-Object 'System.Collections.Generic.List[string]'
    # имена переменных в PowerShell без учёта регистра: $remaining, а не $left (это параметр $Left)
    $remaining = New-Object 'System.Collections.Generic.Dictionary[string,int]'
    foreach ($k in $count.Keys) { $remaining[$k] = $count[$k] }
    for ($i = 0; $i -lt $Left.Lines.Count; $i++) {
        $l = $Left.Lines[$i]
        if ($remaining[$l] -gt 0) { $removed.Add($Left.Raw[$i]); $remaining[$l]-- }
    }
    for ($i = 0; $i -lt $Right.Lines.Count; $i++) {
        $l = $Right.Lines[$i]
        if ($remaining[$l] -lt 0) { $added.Add($Right.Raw[$i]); $remaining[$l]++ }
    }
    $ordered = $true
    if ($removed.Count -eq 0 -and $added.Count -eq 0) {
        for ($i = 0; $i -lt $Left.Lines.Count; $i++) { if ($Left.Lines[$i] -cne $Right.Lines[$i]) { $ordered = $false; break } }
    }
    return [pscustomobject]@{ Removed = $removed; Added = $added; Ordered = $ordered }
}

# Сравнение блоков двух файлов (или отчётов). Возвращает отличия по блокам и счётчики.
function Compare-Blocks($LeftBlocks, $RightBlocks) {
    $rightByKey = @{}
    foreach ($b in $RightBlocks) { $rightByKey[$b.Key] = $b }
    $leftByKey = @{}
    foreach ($b in $LeftBlocks) { $leftByKey[$b.Key] = $b }
    $diffs = New-Object 'System.Collections.Generic.List[object]'
    $onlyLeft = New-Object 'System.Collections.Generic.List[string]'
    $onlyRight = New-Object 'System.Collections.Generic.List[string]'
    $significant = 0
    $volatile = 0
    foreach ($b in $LeftBlocks) {
        if (-not $rightByKey.ContainsKey($b.Key)) {
            if ($b.Lines.Count -gt 0 -or $b.Key -ne '(начало файла)') { $onlyLeft.Add($b.Title) }
            if (-not $b.Volatile) { $significant++ } else { $volatile++ }
            continue
        }
        $r = $rightByKey[$b.Key]
        $c = Compare-LineSets $b $r
        if ($c.Removed.Count -eq 0 -and $c.Added.Count -eq 0 -and $c.Ordered) { continue }
        $isVolatile = $b.Volatile -or $r.Volatile
        if ($isVolatile) { $volatile++ } else { $significant++ }
        $diffs.Add([pscustomobject]@{ Title = $b.Title; Volatile = $isVolatile; Removed = $c.Removed; Added = $c.Added
            Ordered = $c.Ordered })
    }
    # блоки только в результате - не «отличие», а новые данные (например, все службы вместо части, как у скрипта
    # на русских 7/10): считаются отдельно
    $newBlocks = 0
    foreach ($b in $RightBlocks) {
        if ($leftByKey.ContainsKey($b.Key)) { continue }
        if ($b.Lines.Count -eq 0 -and $b.Key -eq '(начало файла)') { continue }
        $onlyRight.Add($b.Title)
        if (-not $b.Volatile) { $newBlocks++ } else { $volatile++ }
    }
    return [pscustomobject]@{ Diffs = $diffs; OnlyLeft = $onlyLeft; OnlyRight = $onlyRight
        Significant = $significant; VolatileCount = $volatile; NewBlocks = $newBlocks }
}


# ---------------------------------------------------------------------------
# Файлы папок
# ---------------------------------------------------------------------------

function Test-Skip([string]$Relative) {
    $name = Split-Path $Relative -Leaf
    foreach ($p in $script:SkipPatterns) { if ($name -like $p) { return $true } }
    return $false
}

# Группа "только наличие" для файла или $null.
function Get-NameGroup([string]$Relative) {
    foreach ($g in $script:NameOnlyGroups) { if ($Relative.StartsWith($g + '\', [StringComparison]::OrdinalIgnoreCase)) { return $g } }
    foreach ($f in $script:NameOnlyFiles) { if ($Relative -ieq $f) { return $f } }
    $ext = [IO.Path]::GetExtension($Relative)
    if ($ext -ne '.txt' -and $ext -ne '.csv') { return '(прочие файлы)' }
    return $null
}

# Файлы папки: путь внутри папки (имя папки <ИМЯ_ПК>_<IP> в именах файлов заменено на %ПАПКА%) -> полный путь.
function Get-FolderFiles([string]$Root, [string]$FolderName) {
    $files = [ordered]@{}
    $rootFull = (Resolve-Path -LiteralPath $Root).Path.TrimEnd('\')
    foreach ($f in (Get-ChildItem -LiteralPath $rootFull -Recurse -File -Force | Sort-Object FullName)) {
        $relative = $f.FullName.Substring($rootFull.Length + 1)
        if (Test-Skip $relative) { continue }
        if ($FolderName) { $relative = $relative.Replace($FolderName, '%ПАПКА%') }
        $files[$relative] = $f.FullName
    }
    return $files
}

# Имя папки утилиты в прогоне vm_test.ps1 (_folder.txt) или имя самой папки.
function Get-FolderName([string]$Path) {
    $flag = Join-Path $Path '_folder.txt'
    if (Test-Path -LiteralPath $flag) {
        $first = Get-Content -LiteralPath $flag | Where-Object { $_.Trim() } | Select-Object -First 1
        if ($first) { return $first.Trim() }
    }
    return (Split-Path $Path -Leaf)
}

function Find-Etalon([string]$FolderName) {
    $hostName = $FolderName
    $k = $FolderName.LastIndexOf('_')
    if ($k -gt 0) { $hostName = $FolderName.Substring(0, $k) }
    $match = Get-ChildItem -LiteralPath $EtalonRoot -Directory | Where-Object { $_.Name -like ($hostName + '_*') } |
        Select-Object -First 1
    if ($match) { return $match.FullName }
    return $null
}

function Test-Finished([string]$Path) {
    if (-not $Path -or -not (Test-Path -LiteralPath $Path)) { return 'нет журнала' }
    $lines = (Read-TextFile $Path).Lines
    if (@($lines | Where-Object { $_ -match ' Finished\s*$' }).Count -gt 0) { return 'есть Finished' }
    return 'нет строки Finished'
}


# ---------------------------------------------------------------------------
# Отчёт
# ---------------------------------------------------------------------------

function Add-DiffText($Md, $Diff) {
    $mark = ''
    if ($Diff.Volatile) { $mark = ' (изменчивый блок, справочно)' }
    if ($Diff.Removed.Count -eq 0 -and $Diff.Added.Count -eq 0) {
        $Md.Add('- `[' + $Diff.Title + ']`' + $mark + ': те же строки, другой порядок')
        return
    }
    $Md.Add('- `[' + $Diff.Title + ']`' + $mark + ': только в эталоне ' + $Diff.Removed.Count + ', только в результате ' +
        $Diff.Added.Count)
    $Md.Add('')
    $Md.Add('  ```')
    $shown = 0
    foreach ($l in $Diff.Removed) { if ($shown -ge $MaxExamples) { break }; $Md.Add('  - ' + $l); $shown++ }
    if ($Diff.Removed.Count -gt $shown) { $Md.Add('  - ... ещё ' + ($Diff.Removed.Count - $shown)) }
    $shown = 0
    foreach ($l in $Diff.Added) { if ($shown -ge $MaxExamples) { break }; $Md.Add('  + ' + $l); $shown++ }
    if ($Diff.Added.Count -gt $shown) { $Md.Add('  + ... ещё ' + ($Diff.Added.Count - $shown)) }
    $Md.Add('  ```')
}

function Add-BlocksText($Md, $Result) {
    if ($Result.OnlyLeft.Count) { $Md.Add('- блоки только в эталоне: ' + (($Result.OnlyLeft | ForEach-Object { '`[' + $_ + ']`' }) -join ', ')) }
    if ($Result.OnlyRight.Count) { $Md.Add('- блоки только в результате: ' + (($Result.OnlyRight | ForEach-Object { '`[' + $_ + ']`' }) -join ', ')) }
    foreach ($d in ($Result.Diffs | Where-Object { -not $_.Volatile })) { Add-DiffText $Md $d }
    foreach ($d in ($Result.Diffs | Where-Object { $_.Volatile })) { Add-DiffText $Md $d }
}

# Отчёты make_md.ps1 по копиям папок: make_md обрабатывает все папки внутри -Path и пишет в них отчёт.
function New-MdReport([string]$Source, [string]$FolderName, [string]$Work) {
    $dst = Join-Path $Work $FolderName
    New-Item -ItemType Directory -Force $dst | Out-Null
    $files = Get-FolderFiles $Source ''
    foreach ($relative in $files.Keys) {
        $target = Join-Path $dst $relative
        New-Item -ItemType Directory -Force (Split-Path $target -Parent) | Out-Null
        Copy-Item -LiteralPath $files[$relative] -Destination $target
    }
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $makeMd -Path $Work *> $null
    $report = Get-ChildItem -LiteralPath $dst -Filter '*_отчёт.md' | Select-Object -First 1
    if ($report) { return $report.FullName }
    return $null
}

function Compare-One([string]$ActualPath) {
    $ActualPath = (Resolve-Path -LiteralPath $ActualPath).Path
    $folderName = Get-FolderName $ActualPath
    $etalonPath = $Etalon
    if (-not $etalonPath) { $etalonPath = Find-Etalon $folderName }
    if (-not $etalonPath -or -not (Test-Path -LiteralPath $etalonPath)) {
        Write-Host "=== СВЕРКА ${folderName}: нет эталона в $EtalonRoot"
        return [pscustomobject]@{ Folder = $folderName; Etalon = ''; Same = 0; Different = 0; Missing = 0; Extra = 0
            BadFormat = 0; ReportDiff = ''; Verdict = 'нет эталона'; ReportPath = '' }
    }
    $etalonPath = (Resolve-Path -LiteralPath $etalonPath).Path
    $etalonName = Split-Path $etalonPath -Leaf
    $outPath = $OutDir
    if (-not $outPath) { $outPath = $ActualPath }
    New-Item -ItemType Directory -Force $outPath | Out-Null

    $leftFiles = Get-FolderFiles $etalonPath $etalonName
    $rightFiles = Get-FolderFiles $ActualPath $folderName
    $rows = New-Object 'System.Collections.Generic.List[object]'
    $details = New-Object 'System.Collections.Generic.List[string]'
    $groups = [ordered]@{}
    $same = 0; $different = 0; $missing = 0; $extra = 0; $badFormat = 0; $withNew = 0

    $allKeys = New-Object 'System.Collections.Generic.List[string]'
    foreach ($k in $leftFiles.Keys) { $allKeys.Add($k) }
    foreach ($k in $rightFiles.Keys) { if (-not $leftFiles.Contains($k)) { $allKeys.Add($k) } }

    foreach ($relative in $allKeys) {
        $group = Get-NameGroup $relative
        $inLeft = $leftFiles.Contains($relative)
        $inRight = $rightFiles.Contains($relative)
        if ($group) {
            if (-not $groups.Contains($group)) { $groups[$group] = [pscustomobject]@{ Left = 0; Right = 0
                Missing = (New-Object 'System.Collections.Generic.List[string]'); Extra = (New-Object 'System.Collections.Generic.List[string]') } }
            $g = $groups[$group]
            if ($inLeft) { $g.Left++ }
            if ($inRight) { $g.Right++ }
            if ($inLeft -and -not $inRight) { $g.Missing.Add((Split-Path $relative -Leaf)) }
            if ($inRight -and -not $inLeft) { $g.Extra.Add((Split-Path $relative -Leaf)) }
            continue
        }
        if (-not $inRight) {
            $missing++
            $rows.Add([pscustomobject]@{ File = $relative; Left = (Read-TextFile $leftFiles[$relative]).Lines.Count; Right = ''
                Format = ''; Verdict = 'нет в результате' })
            continue
        }
        $rightFile = Read-TextFile $rightFiles[$relative]
        $format = Get-FormatText $rightFile
        if ($format.EndsWith('(!)')) { $badFormat++ }
        if (-not $inLeft) {
            $extra++
            $rows.Add([pscustomobject]@{ File = $relative; Left = ''; Right = $rightFile.Lines.Count; Format = $format
                Verdict = 'нет в эталоне' })
            continue
        }
        $leftFile = Read-TextFile $leftFiles[$relative]
        $result = Compare-Blocks (Get-Blocks $leftFile.Lines $false) (Get-Blocks $rightFile.Lines $false)
        $verdict = 'совпадает'
        if ($result.Significant -gt 0) {
            $different++
            $verdict = 'отличается: блоков ' + $result.Significant
        }
        else { $same++ }
        if ($result.NewBlocks -gt 0) { $verdict += '; новых блоков ' + $result.NewBlocks; $withNew++ }
        if ($result.VolatileCount -gt 0) { $verdict += '; изменчивых ' + $result.VolatileCount }
        $rows.Add([pscustomobject]@{ File = $relative; Left = $leftFile.Lines.Count; Right = $rightFile.Lines.Count
            Format = $format; Verdict = $verdict })
        if ($result.Significant -gt 0 -or $result.VolatileCount -gt 0 -or $result.NewBlocks -gt 0) {
            $details.Add('')
            $details.Add('### ' + $relative)
            $details.Add('')
            Add-BlocksText $details $result
        }
    }

    # отчёты make_md.ps1
    $reportVerdict = ''
    $reportDetails = New-Object 'System.Collections.Generic.List[string]'
    if ($Report) {
        $work = Join-Path ([IO.Path]::GetTempPath()) ('compare_collect_' + [Guid]::NewGuid().ToString('N'))
        try {
            $leftReport = New-MdReport $etalonPath $etalonName (Join-Path $work 'etalon')
            $rightReport = New-MdReport $ActualPath $folderName (Join-Path $work 'actual')
            if (-not $leftReport -or -not $rightReport) { $reportVerdict = 'make_md.ps1 не построил отчёт' }
            else {
                Copy-Item -LiteralPath $leftReport -Destination (Join-Path $outPath '_отчёт_эталон.md') -Force
                Copy-Item -LiteralPath $rightReport -Destination (Join-Path $outPath '_отчёт_результат.md') -Force
                $result = Compare-Blocks (Get-Blocks (Read-TextFile $leftReport).Lines $true) (Get-Blocks (Read-TextFile $rightReport).Lines $true)
                if ($result.Significant -eq 0) { $reportVerdict = 'совпадает' }
                else { $reportVerdict = 'отличается: разделов ' + $result.Significant }
                if ($result.NewBlocks -gt 0) { $reportVerdict += ', новых разделов ' + $result.NewBlocks }
                if ($result.Significant -gt 0 -or $result.NewBlocks -gt 0) { Add-BlocksText $reportDetails $result }
            }
        }
        finally {
            if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue }
        }
    }

    $md = New-Object 'System.Collections.Generic.List[string]'
    $md.Add('# Сверка с эталоном: ' + $folderName)
    $md.Add('')
    $md.Add('Сформировано ' + (Get-Date -Format 'dd.MM.yyyy HH:mm') + ' скриптом `vmtest\compare_collect.ps1`.')
    $md.Add('')
    $md.Add('- эталон: `' + $etalonPath + '` (журнал: ' + (Test-Finished $leftFiles['_журнал_сбора.txt']) + ')')
    $md.Add('- результат: `' + $ActualPath + '` (журнал: ' + (Test-Finished $rightFiles['_журнал_сбора.txt']) + ')')
    $mode = 'пробелы схлопнуты, пустые строки не считаются'
    if ($Exact) { $mode = 'строго, только пробелы в конце строк не считаются' }
    $md.Add('- сравнение: по блокам, без учёта порядка строк; дата и время замаскированы; ' + $mode)
    $md.Add('- итог: совпадает ' + $same + ' (из них с новыми блоками ' + $withNew + '), отличается ' + $different +
        ', нет в результате ' + $missing + ', нет в эталоне ' + $extra + ', формат не UTF-8 с BOM и CRLF: ' + $badFormat)
    $md.Add('- «отличается» - в блоке другие строки или блока нет в результате; блоки только в результате - «новые»,' +
        ' в отличия не входят; изменчивые блоки (числа замаскированы) - справочно')
    if ($Report) { $md.Add('- отчёт make_md.ps1: ' + $reportVerdict) }
    $md.Add('')
    $md.Add('| Файл | Строк в эталоне | Строк в результате | Формат результата | Итог |')
    $md.Add('|---|---|---|---|---|')
    foreach ($r in $rows) { $md.Add('| ' + $r.File + ' | ' + $r.Left + ' | ' + $r.Right + ' | ' + $r.Format + ' | ' + $r.Verdict + ' |') }
    if ($groups.Count) {
        $md.Add('')
        $md.Add('Сверяется только наличие файлов:')
        $md.Add('')
        $md.Add('| Папка или файл | В эталоне | В результате | Нет в результате | Нет в эталоне |')
        $md.Add('|---|---|---|---|---|')
        foreach ($name in $groups.Keys) {
            $g = $groups[$name]
            $md.Add('| ' + $name + ' | ' + $g.Left + ' | ' + $g.Right + ' | ' + ($g.Missing -join ', ') + ' | ' + ($g.Extra -join ', ') + ' |')
        }
    }
    if ($details.Count) {
        $md.Add('')
        $md.Add('## Отличия по файлам')
        foreach ($l in $details) { $md.Add($l) }
    }
    if ($reportDetails.Count) {
        $md.Add('')
        $md.Add('## Отличия отчётов make_md.ps1')
        $md.Add('')
        $md.Add('Отчёты целиком - `_отчёт_эталон.md` и `_отчёт_результат.md` рядом с этим файлом.')
        $md.Add('')
        foreach ($l in $reportDetails) { $md.Add($l) }
    }
    $reportPath = Join-Path $outPath '_сверка_с_эталоном.md'
    [IO.File]::WriteAllLines($reportPath, $md, (New-Object Text.UTF8Encoding $true))

    $verdict = "совпадает $same (с новыми блоками $withNew), отличается $different, нет в результате $missing, " +
        "нет в эталоне $extra, формат (!) $badFormat"
    if ($Report) { $verdict += ", отчёт make_md: $reportVerdict" }
    Write-Host "=== СВЕРКА ${folderName}: $verdict"
    return [pscustomobject]@{ Folder = $folderName; Etalon = $etalonPath; Same = $same; Different = $different
        Missing = $missing; Extra = $extra; BadFormat = $badFormat; ReportDiff = $reportVerdict; Verdict = $verdict
        ReportPath = $reportPath }
}

[Console]::OutputEncoding = New-Object Text.UTF8Encoding $false
$targets = @($Actual -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ })
if ($Etalon -and $targets.Count -gt 1) { throw '-Etalon задаётся только для одной папки -Actual' }
foreach ($t in $targets) { Compare-One $t }
