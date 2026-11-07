# Итоговая папка тестов: по каждой ВМ - последний успешный прогон (exitcode 0) из vmtest\results.
# Кладёт в vmtest\итог_<Name>\<ВМ>\ все PNG и журнал прогона, в корень - итог.md с таблицей.
# Пример: .\make_summary.ps1 -Name этап1
param(
    [string]$Name = 'этап1',
    [string[]]$Extra = @(),         # папки прогонов из results для раздела "Дополнительные проверки" (любой код возврата)
    [string[]]$Vms = @('audit_xpsp2', 'audit_xpsp3vl', 'audit_xpx64sp2', 'audit_win7ent', 'audit_win7ent_ru', 'audit_win10pro')
)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$res = Join-Path $here 'results'
$out = Join-Path $here "итог_$Name"
$logName = '_журнал_скриншотов.txt'

if (Test-Path $out) { Remove-Item $out -Recurse -Force }
New-Item -ItemType Directory -Force $out | Out-Null

$rows = @()
foreach ($vm in $Vms) {
    # имена папок: <ВМ>_ггггММдд_ЧЧммсс; audit_win7ent не должна цеплять audit_win7ent_ru
    # прогоны из -Extra (другое разрешение и т. п.) в основную таблицу не берутся
    $runs = Get-ChildItem $res -Directory | Where-Object { $_.Name -match ('^' + [regex]::Escape($vm) + '_\d{8}_\d{6}$') -and $Extra -notcontains $_.Name } | Sort-Object Name -Descending
    $run = $runs | Where-Object { (Get-Content (Join-Path $_.FullName 'exitcode.txt') -ErrorAction SilentlyContinue | Select-Object -First 1) -match '^\s*0\s*$' } | Select-Object -First 1
    if (-not $run) {
        $rows += [pscustomobject]@{ Vm = $vm; Run = '-'; Os = ''; Exe = ''; Screen = ''; Tip = 'нет успешного прогона'; Files = '' }
        continue
    }
    $dst = Join-Path $out $vm
    New-Item -ItemType Directory -Force $dst | Out-Null
    # снимки утилиты; служебные снимки экрана гостя (guest_*) не берём
    $pngs = Get-ChildItem $run.FullName -Filter *.png | Where-Object { $_.Name -notlike 'guest_*' }
    $pngs | Copy-Item -Destination $dst
    $log = Join-Path $run.FullName $logName
    $text = ''
    if (Test-Path $log) { Copy-Item $log $dst; $text = Get-Content $log -Encoding UTF8 -Raw }
    $os = if ($text -match 'ОС: ([^\r\n]+)') { $Matches[1].Trim() } else { '' }
    $exe = if ($text -match 'Сборка утилиты: (\S+)') { $Matches[1] } else { '' }
    $scr = if ($text -match 'Экран: (\d+x\d+)') { $Matches[1] } else { '' }
    $tip = if ($text -match 'подсказка появилась через (\d+) мс \(класс ([^,]+)') { "$($Matches[2]), $($Matches[1]) мс" }
           elseif ($text -match 'подсказка не появилась') { 'НЕ появилась' } else { '' }
    if ($text -match 'Спрятано всплывающих уведомлений') { $tip += '; шарики спрятаны' }
    $rows += [pscustomobject]@{ Vm = $vm; Run = $run.Name; Os = $os; Exe = $exe; Screen = $scr; Tip = $tip; Files = ($pngs.Name -join ', ') }
}

$md = @()
$md += "# Итог тестов screens.exe: $Name"
$md += ''
$md += "Собрано $(Get-Date -Format 'dd.MM.yyyy HH:mm'). По каждой ВМ - последний прогон с кодом возврата 0 из ``vmtest\results``."
$md += 'В папке ВМ - снимки утилиты и её журнал.'
$md += ''
$md += '| ВМ | ОС (из журнала) | Сборка | Экран | Подсказка у часов | Снимки | Прогон |'
$md += '|---|---|---|---|---|---|---|'
foreach ($r in $rows) { $md += "| $($r.Vm) | $($r.Os) | $($r.Exe) | $($r.Screen) | $($r.Tip) | $($r.Files) | $($r.Run) |" }
if ($Extra.Count) {
    $md += ''
    $md += '## Дополнительные проверки'
    $md += ''
    $md += '| Прогон | Аргументы | Код | Итог журнала |'
    $md += '|---|---|---|---|'
    foreach ($e in $Extra) {
        $src = Join-Path $res $e
        if (-not (Test-Path $src)) { $md += "| $e | | | нет такой папки |"; continue }
        $dst = Join-Path $out "доп_$e"
        New-Item -ItemType Directory -Force $dst | Out-Null
        Get-ChildItem $src -File | Where-Object { $_.Name -ne 'done.txt' -and $_.Name -ne 'started.txt' } | Copy-Item -Destination $dst
        $text = Get-Content (Join-Path $src $logName) -Encoding UTF8 -Raw -ErrorAction SilentlyContinue
        $argsLine = if ($text -match 'запуск: [^\r\n]*?"[^"]*"\s+"[^"]*"\s*([^\r\n]*)') { $Matches[1].Trim() } else { '' }
        $code = (Get-Content (Join-Path $src 'exitcode.txt') -ErrorAction SilentlyContinue | Select-Object -First 1)
        $key = @(($text -split "`r?`n") | Where-Object { $_ -match 'СТОРОЖ|закрыто|Завершено|ОШИБКА' } | ForEach-Object { ($_ -replace '^\S+ \S+\s+', '') }) -join '; '
        $md += "| $e | $argsLine | $code | $key |"
    }
}
[IO.File]::WriteAllLines((Join-Path $out 'итог.md'), $md, (New-Object Text.UTF8Encoding $true))
$rows | Format-Table Vm, Exe, Screen, Tip, Run -AutoSize | Out-String -Width 200
"Итог: $out"
