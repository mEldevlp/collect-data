# Итоговая папка тестов: по каждой ВМ - последний успешный прогон (exitcode 0) из vmtest\results.
# Кладёт в vmtest\итог_<Name>\<ВМ>\ все PNG и журнал прогона, в корень - итог.md с таблицей.
# -Collect - сбор данных: берётся последний успешный прогон, в котором есть _журнал_сбора.txt, он сверяется
#  с эталоном (compare_collect.ps1 -Report); в папку ВМ кладутся журнал сбора, сверка и отчёт make_md.ps1.
# Пример: .\make_summary.ps1 -Name этап1
param(
    [string]$Name = 'этап1',
    [string[]]$Extra = @(),         # папки прогонов из results для раздела "Дополнительные проверки" (любой код возврата)
    [string[]]$Vms = @('audit_xpsp2', 'audit_xpsp3vl', 'audit_xpx64sp2', 'audit_win7ent', 'audit_win7ent_ru', 'audit_win10pro'),
    [switch]$Collect,               # сбор данных: сверка с эталоном
    [string]$EtalonRoot = ''        # эталоны для -Collect; по умолчанию ..\..\VMs (см. compare_collect.ps1)
)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$res = Join-Path $here 'results'
$out = Join-Path $here "итог_$Name"
$logName = '_журнал_скриншотов.txt'
$collectLogName = '_журнал_сбора.txt'
$compare = Join-Path $here 'compare_collect.ps1'

# Длительность сбора по журналу: от первой до последней строки с датой и временем.
function Get-CollectDuration([string]$Log) {
    $stamps = @()
    foreach ($l in (Get-Content -LiteralPath $Log -Encoding UTF8)) {
        # %TIME% в cmd до 10 часов - с пробелом вместо нуля: "04.11.2026  9:05:01,23"
        if ($l -match '^﻿?(\d{2}\.\d{2}\.\d{4})\s+(\d{1,2}:\d{2}:\d{2})') {
            $stamps += [datetime]::ParseExact(($Matches[1] + ' ' + $Matches[2].PadLeft(8, '0')), 'dd.MM.yyyy HH:mm:ss', $null)
        }
    }
    if ($stamps.Count -lt 2) { return '' }
    $span = $stamps[$stamps.Count - 1] - $stamps[0]
    return ('{0}:{1:00}' -f [int][Math]::Floor($span.TotalMinutes), $span.Seconds)
}

if (Test-Path $out) { Remove-Item $out -Recurse -Force }
New-Item -ItemType Directory -Force $out | Out-Null

$rows = @()
foreach ($vm in $Vms) {
    # имена папок: <ВМ>_ггггММдд_ЧЧммсс; audit_win7ent не должна цеплять audit_win7ent_ru
    # прогоны из -Extra (другое разрешение и т. п.) в основную таблицу не берутся
    $runs = Get-ChildItem $res -Directory | Where-Object { $_.Name -match ('^' + [regex]::Escape($vm) + '_\d{8}_\d{6}$') -and $Extra -notcontains $_.Name } | Sort-Object Name -Descending
    $run = $runs | Where-Object { (Get-Content (Join-Path $_.FullName 'exitcode.txt') -ErrorAction SilentlyContinue | Select-Object -First 1) -match '^\s*0\s*$' } |
        Where-Object { -not $Collect -or (Test-Path (Join-Path $_.FullName $collectLogName)) } | Select-Object -First 1
    if (-not $run) {
        $rows += [pscustomobject]@{ Vm = $vm; Run = '-'; Os = ''; Exe = ''; Screen = ''; Tip = 'нет успешного прогона'; Files = ''
            Collect = ''; Duration = '' }
        continue
    }
    $dst = Join-Path $out $vm
    New-Item -ItemType Directory -Force $dst | Out-Null
    $collectText = ''
    $duration = ''
    if ($Collect) {
        $collectLog = Join-Path $run.FullName $collectLogName
        Copy-Item -LiteralPath $collectLog $dst
        $duration = Get-CollectDuration $collectLog
        $a = @{ Actual = $run.FullName; OutDir = $dst; Report = $true }
        if ($EtalonRoot) { $a.EtalonRoot = $EtalonRoot }
        $c = & $compare @a 6>$null | Select-Object -Last 1
        # отчёт make_md по эталону в итог не кладём: он есть в самом эталоне и одинаков для всех частей
        Remove-Item -LiteralPath (Join-Path $dst '_отчёт_эталон.md') -ErrorAction SilentlyContinue
        $collectText = $c.Verdict
    }
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
    $rows += [pscustomobject]@{ Vm = $vm; Run = $run.Name; Os = $os; Exe = $exe; Screen = $scr; Tip = $tip; Files = ($pngs.Name -join ', ')
        Collect = $collectText; Duration = $duration }
}

$md = @()
$md += "# Итог тестов screens.exe: $Name"
$md += ''
$md += "Собрано $(Get-Date -Format 'dd.MM.yyyy HH:mm'). По каждой ВМ - последний прогон с кодом возврата 0 из ``vmtest\results``."
$md += 'В папке ВМ - снимки утилиты и её журнал.'
if ($Collect) { $md += 'Сбор данных: в папке ВМ - журнал сбора, сверка с эталоном (`_сверка_с_эталоном.md`) и отчёт make_md.ps1 (`_отчёт_результат.md`).' }
$md += ''
if ($Collect) {
    $md += '| ВМ | Сбор, мин:с | Сверка с эталоном | ОС (из журнала снимков) | Снимки | Прогон |'
    $md += '|---|---|---|---|---|---|'
    foreach ($r in $rows) { $md += "| $($r.Vm) | $($r.Duration) | $($r.Collect) | $($r.Os) | $($r.Files) | $($r.Run) |" }
}
else {
    $md += '| ВМ | ОС (из журнала) | Сборка | Экран | Подсказка у часов | Снимки | Прогон |'
    $md += '|---|---|---|---|---|---|---|'
    foreach ($r in $rows) { $md += "| $($r.Vm) | $($r.Os) | $($r.Exe) | $($r.Screen) | $($r.Tip) | $($r.Files) | $($r.Run) |" }
}
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
if ($Collect) { $rows | Format-Table Vm, Duration, Collect, Run -AutoSize | Out-String -Width 300 }
else { $rows | Format-Table Vm, Exe, Screen, Tip, Run -AutoSize | Out-String -Width 200 }
"Итог: $out"
