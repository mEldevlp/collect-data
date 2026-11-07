# Серия прогонов vm_test.ps1 по нескольким ВМ подряд (по одной), вывод - в консоль и в журнал (UTF-8).
# Строки "=== <ВМ> START/END", "=== SERIES DONE" - для слежения за серией (grep/Monitor).
# exe копируется в общую папку в начале прогона каждой ВМ: не пересобирать, пока серия не дошла до последней ВМ.
# Пример: .\vm_series.ps1 -Vms 'audit_xpsp2,audit_win10pro' -ScrArgs '/only:sync_time /dump' -LogFile C:\temp\series.log
param(
    [string]$Vms = 'audit_xpsp2,audit_xpsp3vl,audit_xpx64sp2,audit_win7ent,audit_win7ent_ru,audit_win10pro',
    [string]$ScrArgs = '/only:test',
    [string]$Prep = '',
    [string]$Resolution = '1024x768',
    [string]$LogFile = ''
)
# иначе PowerShell 5.1 отдаёт кириллицу в канал в кодировке консоли, и grep её не находит
[Console]::OutputEncoding = New-Object Text.UTF8Encoding $false
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$t = Join-Path $here 'vm_test.ps1'
if (-not $LogFile) { $LogFile = Join-Path $here ("results\series_{0:yyyyMMdd_HHmmss}.log" -f (Get-Date)) }
function Out2($s) { $s; Add-Content -Path $LogFile -Value $s -Encoding UTF8 }
foreach ($vm in ($Vms -split ',')) {
    Out2 "=== $vm START $(Get-Date -Format HH:mm:ss)"
    try {
        $a = @{ Vm = $vm; ScrArgs = $ScrArgs; Resolution = $Resolution }
        if ($Prep) { $a.Prep = $Prep }
        & $t @a *>&1 | ForEach-Object { Out2 "$_" }
    } catch {
        Out2 "=== $vm EXCEPTION: $_"
    }
    Out2 "=== $vm END $(Get-Date -Format HH:mm:ss)"
}
Out2 '=== SERIES DONE'
