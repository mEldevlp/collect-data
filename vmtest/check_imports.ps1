# Проверка: все статически импортируемые функции exe есть в DLL Windows XP SP2.
# DLL XP лежат в vmtest\xpdlls\xp_sp2_x86 и xp_sp2_x64 (распакованы из ISO дистрибутивов).
# Пример: .\check_imports.ps1 ..\..\bin\screens_x86.exe
# Код возврата: 0 - всё есть, 1 - есть отсутствующие функции, 2 - ошибка проверки.
param([Parameter(Mandatory = $true)][string[]]$Exe)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$dumpbin = & $vswhere -latest -prerelease -products * -find 'VC\Tools\MSVC\**\bin\Hostx64\x64\dumpbin.exe' | Select-Object -First 1
if (-not $dumpbin) { Write-Host 'Не найден dumpbin.exe'; exit 2 }

function Get-Imports($path) {
    # DLL -> список имён функций
    $res = @{}
    $cur = $null
    foreach ($l in (& $dumpbin /nologo /imports $path)) {
        if ($l -match '^\s+Summary\s*$') { break }
        if ($l -match '^\s{4}(\S+\.dll)\s*$') { $cur = $Matches[1].ToLower(); $res[$cur] = New-Object System.Collections.ArrayList; continue }
        if ($cur -and $l -match '^\s+[0-9A-F]+\s+(\S+)\s*$') { [void]$res[$cur].Add($Matches[1]) }
        elseif ($cur -and $l -match '^\s+Ordinal\s+(\d+)') { [void]$res[$cur].Add("#" + $Matches[1]) }
    }
    $res
}
$exportCache = @{}
function Get-Exports($dll) {
    if ($exportCache.ContainsKey($dll)) { return $exportCache[$dll] }
    $set = New-Object 'System.Collections.Generic.HashSet[string]'
    $inTable = $false
    foreach ($l in (& $dumpbin /nologo /exports $dll)) {
        if ($l -match '^\s+ordinal\s+hint\s+RVA\s+name') { $inTable = $true; continue }
        if (-not $inTable) { continue }
        if ($l -match '^\s+(\d+)\s+[0-9A-F]+\s+(?:[0-9A-F]{8}\s+)?(\S+)') { [void]$set.Add($Matches[2]); [void]$set.Add('#' + $Matches[1]) }
        elseif ($l -match '^\s+(\d+)\s+(\S+)\s+\(forwarded') { [void]$set.Add($Matches[2]) }
        elseif ($l -match '^\s+(\d+)\s+[0-9A-F]+\s+\[NONAME\]') { [void]$set.Add('#' + $Matches[1]) }
    }
    $exportCache[$dll] = $set
    $set
}

$rc = 0
foreach ($e in $Exe) {
    $e = (Resolve-Path $e).Path
    $hdr = & $dumpbin /nologo /headers $e
    $is64 = [bool]($hdr | Select-String 'machine \(x64\)')
    $xpdir = Join-Path $here ($(if ($is64) { 'xpdlls\xp_sp2_x64' } else { 'xpdlls\xp_sp2_x86' }))
    $subsys = ($hdr | Select-String '([\d.]+) subsystem version').Matches[0].Groups[1].Value
    $osver = ($hdr | Select-String '([\d.]+) operating system version').Matches[0].Groups[1].Value
    Write-Host "== $e ($(if ($is64) {'x64'} else {'x86'}))"
    $need = $(if ($is64) { '5.02' } else { '5.01' })
    Write-Host "   версия подсистемы $subsys, версия ОС $osver (нужно не выше $need)"
    if ([version]$subsys -gt [version]$need) { Write-Host '   ОШИБКА: XP не запустит этот exe'; $rc = 1 }
    $imp = Get-Imports $e
    $missing = 0
    foreach ($dll in ($imp.Keys | Sort-Object)) {
        $p = Join-Path $xpdir $dll
        if (-not (Test-Path $p)) { Write-Host "   ${dll}: нет такой DLL в $xpdir - не проверено"; $rc = [Math]::Max($rc, 1); continue }
        $ex = Get-Exports $p
        $bad = @($imp[$dll] | Where-Object { -not $ex.Contains($_) })
        Write-Host ("   {0}: {1} функций{2}" -f $dll, $imp[$dll].Count, $(if ($bad.Count) { ", НЕТ В XP SP2: " + ($bad -join ', ') } else { ', все есть в XP SP2' }))
        $missing += $bad.Count
    }
    if ($missing) { $rc = 1 }
}
exit $rc
