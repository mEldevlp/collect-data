# Тестовый прогон screens.exe в ВМ VirtualBox.
#  1) откат к снимку clean (если не -NoRestore) и запуск ВМ;
#  2) ожидание входа пользователя (GuestAdditionsRunLevel = 3);
#  3) временная общая папка "screens" = vmtest\share, туда кладутся exe и args.txt;
#  4) нажатиями клавиш (чтобы процесс был на интерактивном столе) запуск \\vboxsrv\screens\run.cmd;
#     на 7/10 - с повышением прав через UAC, как collect_audit.cmd у пользователя;
#     имя vboxsrv, а не vboxsvr: GA на Win7 не знают vboxsvr;
#  5) ожидание results\<ИМЯ_ПК>\done.txt, снимок экрана гостя, перенос результатов в vmtest\results\;
#  6) выключение ВМ (если не -Keep).
# Пример: .\vm_test.ps1 -Vm audit_xpsp2 -ScrArgs '/only:test'
param(
    [Parameter(Mandatory = $true)][string]$Vm,
    [string]$ScrArgs = '/only:test',
    [string]$Prep = '',              # .cmd, который run.cmd выполнит в госте перед утилитой (тестовые данные)
    [switch]$Trial,                 # пробная сборка из obj\v145 (не для XP)
    [string]$ExeDir = '',            # где взять screens_x86/x64.exe; по умолчанию ..\..\bin
    [switch]$NoRestore,              # не откатывать к clean (ВМ уже запущена/подготовлена)
    [switch]$Keep,                   # не выключать ВМ после прогона
    [string]$Resolution = '1024x768', # разрешение гостя (setvideomodehint); '' - не менять
    [ValidateSet('auto','none','win7','win10')][string]$Uac = 'auto', # запуск с повышением: auto - none для XP, win7/win10 по имени ВМ
    [int]$BootTimeoutSec = 900,
    [int]$RunTimeoutSec = 600
)
$ErrorActionPreference = 'Stop'
$vb = 'C:\Program Files\Oracle\VirtualBox\VBoxManage.exe'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$share = Join-Path $here 'share'
if (-not $ExeDir) { $ExeDir = Join-Path $here '..\..\bin' }
if (Test-Path $ExeDir) { $ExeDir = (Resolve-Path $ExeDir).Path }

function Log($m) { Write-Host ("[{0:HH:mm:ss}] {1}" -f (Get-Date), $m) }
# stderr VBoxManage (полоса прогресса) в PS 5.1 при Stop превращается в ошибку - глушим
function VB { $ErrorActionPreference = 'Continue'; & $vb @args 2>&1 | ForEach-Object { "$_" } | Out-String }
function VmInfo {
    $h = @{}
    foreach ($l in (& $vb showvminfo $Vm --machinereadable 2>$null)) {
        if ($l -match '^"?([^"=]+)"?="?(.*?)"?$') { $h[$Matches[1]] = $Matches[2] }
    }
    $h
}
function Shot($name) {
    $p = Join-Path $script:resDir $name
    & $vb controlvm $Vm screenshotpng $p 2>&1 | Out-Null
    if (Test-Path $p) { Log "Снимок экрана гостя: $p" }
}

# ---- папка результатов прогона ----
$stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
$script:resDir = Join-Path $here "results\${Vm}_$stamp"
New-Item -ItemType Directory -Force $script:resDir | Out-Null

# ---- exe и аргументы в общую папку ----
foreach ($e in 'screens_x86.exe', 'screens_x64.exe') {
    $src = Join-Path $ExeDir $e
    if ($Trial) { $src = Join-Path $here ("..\obj\v145\Release_{0}\{1}" -f $e.Substring(8,3), $e) }
    if (Test-Path $src) { Copy-Item $src $share -Force } else { Log "Нет $src" }
}
Set-Content -Path (Join-Path $share 'args.txt') -Value $ScrArgs -Encoding Ascii
$prepDst = Join-Path $share 'prep.cmd'
if ($Prep) { Copy-Item $Prep $prepDst -Force; Log "Подготовка в госте: $Prep" } elseif (Test-Path $prepDst) { Remove-Item $prepDst -Force }
$guestRes = Join-Path $share 'results'
# содержимое, а не саму папку: её может держать открытой проводник/консоль
if (Test-Path $guestRes) { Get-ChildItem $guestRes -Force | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue }
New-Item -ItemType Directory -Force $guestRes | Out-Null

# ---- запуск ВМ ----
$info = VmInfo
if (-not $NoRestore) {
    if ($info.VMState -ne 'poweroff' -and $info.VMState -ne 'aborted' -and $info.VMState -ne 'saved') {
        Log "ВМ в состоянии $($info.VMState), выключаю"
        VB controlvm $Vm poweroff | Out-Null
        Start-Sleep 3
    }
    Log "Откат к снимку clean"
    VB snapshot $Vm restore clean | Out-Null
    $info = VmInfo
}
if ($info.VMState -ne 'running') {
    Log "Запуск ВМ $Vm"
    $o = VB startvm $Vm --type headless
    if ($LASTEXITCODE -ne 0) { throw "startvm: $o" }
}

# ---- общая папка: сразу после старта - в Win7 она становится видна интерактивной сессии лишь через 1-2 мин ----
$o = VB sharedfolder add $Vm --name screens --hostpath $share --transient
if ($o -match 'error' -and $o -notmatch 'already exists') { Log "sharedfolder: $o" }

# ---- ожидание входа пользователя ----
$t0 = Get-Date
while ($true) {
    $info = VmInfo
    if ($info.GuestAdditionsRunLevel -eq '3') { break }
    if (((Get-Date) - $t0).TotalSeconds -gt $BootTimeoutSec) {
        Shot 'guest_boot_timeout.png'
        throw "Не дождался входа пользователя за $BootTimeoutSec с (RunLevel=$($info.GuestAdditionsRunLevel))"
    }
    Start-Sleep 5
}
Log ("Пользователь вошёл через {0:N0} с, жду загрузки оболочки" -f ((Get-Date) - $t0).TotalSeconds)
if ($Resolution -match '^(\d+)x(\d+)$') {
    Log "Разрешение гостя $Resolution"
    VB controlvm $Vm setvideomodehint $Matches[1] $Matches[2] 32 | Out-Null
}
Start-Sleep 20


# ---- Win+R и запуск ----
Shot 'guest_before.png'
# На медленной XP окно "Запуск программы" может открыться позже, чем уйдёт текст.
# run.cmd первым делом пишет started.txt; нет его за 150 с - Esc и повтор (до 3 попыток).
# Виден ли запрос UAC Win10: в центре - светло-серое тело диалога (~230,230,230), по краю - затемнённый
# фон (яркость < 80). Простое падение яркости не годится: большое чёрное окно cmd тоже "затемняет" (проверено).
Add-Type -AssemblyName System.Drawing
function UacVisible {
    $f = Join-Path $env:TEMP "vmtest_$Vm.png"
    & $vb controlvm $Vm screenshotpng $f 2>&1 | Out-Null
    if (-not (Test-Path $f)) { return $false }
    $b = [Drawing.Bitmap]::FromFile($f)
    $c = $b.GetPixel([int]($b.Width / 2), [int]($b.Height * 0.585))
    $e = $b.GetPixel([int]($b.Width * 0.2), [int]($b.Height * 0.26))
    $b.Dispose(); Remove-Item $f -ErrorAction SilentlyContinue
    $grey = $c.R -gt 200 -and [Math]::Abs($c.R - $c.G) -lt 12 -and [Math]::Abs($c.G - $c.B) -lt 12
    $dim = (($e.R + $e.G + $e.B) / 3) -lt 80
    return ($grey -and $dim)
}
function WaitUac($sec) {
    $t0 = Get-Date
    while (((Get-Date) - $t0).TotalSeconds -lt $sec) {
        Start-Sleep 2
        if (UacVisible) { Log ("UAC через {0:N0} с" -f ((Get-Date) - $t0).TotalSeconds); Start-Sleep 1; return $true }
    }
    return $false
}# Ctrl+Shift+Enter: модификаторы отдельно и с паузами - одной пачкой скан-кодов окно "Выполнить"
# Win10 принимает это за простой Enter и запускает cmd без повышения (проверено)
function CtrlShiftEnter {
    VB controlvm $Vm keyboardputscancode 1d | Out-Null; Start-Sleep -Milliseconds 300
    VB controlvm $Vm keyboardputscancode 2a | Out-Null; Start-Sleep -Milliseconds 300
    VB controlvm $Vm keyboardputscancode 1c 9c | Out-Null; Start-Sleep -Milliseconds 300
    VB controlvm $Vm keyboardputscancode aa | Out-Null; Start-Sleep -Milliseconds 100
    VB controlvm $Vm keyboardputscancode 9d | Out-Null
}
$uacMode = $Uac
if ($uacMode -eq 'auto') { $uacMode = if ($Vm -match 'xp') { 'none' } elseif ($Vm -match 'win7') { 'win7' } else { 'win10' } }
$started = $false
for ($try = 1; $try -le 3 -and -not $started; $try++) {
    Log "Запуск run.cmd, UAC: $uacMode (попытка $try)"
    VB controlvm $Vm keyboardputscancode 01 81 | Out-Null               # Esc - закрыть недонабранное окно
    Start-Sleep 1
    # Ждём до 90 с, пока run.cmd станет виден: новая общая папка видна в сеансе пользователя не сразу (Win7 - обычный cmd, Win10 - повышенный; 1-2 мин после подключения).
    # net use: Win10 (и обычный, и повышенный cmd) без него \\vboxsrv не видит; на Win7 в повышенном cmd
    # он висит (проверено), а if exist видит папку сразу - там net use не делаем. <nul - не ждать ввода имени.
    $runLine = '(for /l %i in (1,1,90) do @if not exist \\vboxsrv\screens\run.cmd ping -n 2 127.0.0.1 >nul) & \\vboxsrv\screens\run.cmd'
    if ($uacMode -ne 'win7') { $runLine = 'net use \\vboxsrv\screens <nul >nul 2>&1 & ' + $runLine }
    if ($uacMode -eq 'win7') {
        # Win7: в окне "Выполнить" Ctrl+Shift+Enter не повышает - через поиск в "Пуске"
        VB controlvm $Vm keyboardputscancode e0 5b e0 db | Out-Null     # Win - меню "Пуск"
        Start-Sleep 3
        VB controlvm $Vm keyboardputstring 'cmd' | Out-Null
        Start-Sleep 3
        CtrlShiftEnter
        Start-Sleep 6
        VB controlvm $Vm keyboardputscancode 2a 0f 8f aa | Out-Null     # UAC: фокус на "Нет" -> Shift+Tab на "Да"
        Start-Sleep 1
        VB controlvm $Vm keyboardputscancode 1c 9c | Out-Null
        Start-Sleep 5
        VB controlvm $Vm keyboardputstring $runLine | Out-Null
        Start-Sleep 1
        VB controlvm $Vm keyboardputscancode 1c 9c | Out-Null
    } else {
        VB controlvm $Vm keyboardputscancode e0 5b 13 93 e0 db | Out-Null   # LWin+R
        Start-Sleep 6
        if ($uacMode -eq 'win10') {
            # Ctrl+Shift+Enter в окне "Выполнить" Win10 повышает через раз (проверено) - без модификаторов:
            # PowerShell Start-Process -Verb RunAs всегда показывает UAC. Сокращения (saps, -Args, -w, -c) - потому что
            # поле окна "Выполнить" не длиннее 259 символов: длинная строка обрезалась и молча не работала (проверено)
            VB controlvm $Vm keyboardputstring ("powershell -w hidden -c `"saps cmd -Verb RunAs -Args '/c " + $runLine + "'`"") | Out-Null
            Start-Sleep 1
            VB controlvm $Vm keyboardputscancode 1c 9c | Out-Null             # Enter
            # Первый запуск PowerShell после загрузки бывает долгим: ждём сам UAC (экран затемняется), а не фиксированную паузу
            if (-not (WaitUac 45)) { Log 'UAC не замечен за 45 с, жму "Да" наугад' }
            Start-Sleep 1
            VB controlvm $Vm keyboardputscancode e0 4b e0 cb | Out-Null       # UAC: фокус на "Нет" -> стрелка влево на "Да"
            Start-Sleep 1
            VB controlvm $Vm keyboardputscancode 1c 9c | Out-Null
        } else {
            # через cmd /c: прямой запуск .cmd с UNC на Win7 даёт "Open File - Security Warning"
            VB controlvm $Vm keyboardputstring ('cmd /c ' + $runLine) | Out-Null
            Start-Sleep 1
            VB controlvm $Vm keyboardputscancode 1c 9c | Out-Null             # Enter
        }
    }
    $t1 = Get-Date
    while (((Get-Date) - $t1).TotalSeconds -lt 150) {
        if (Get-ChildItem $guestRes -Recurse -Filter started.txt -ErrorAction SilentlyContinue) { $started = $true; break }
        Start-Sleep 2
    }
    if (-not $started) { Shot "guest_try$try.png" }
}
if (-not $started) { Log 'run.cmd так и не запустился' }

# ---- ожидание результата ----
$t0 = Get-Date
$done = $null
while (-not $done) {
    $done = Get-ChildItem $guestRes -Recurse -Filter done.txt -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($done) { break }
    if (((Get-Date) - $t0).TotalSeconds -gt $RunTimeoutSec) {
        Shot 'guest_run_timeout.png'
        Log "Нет результата за $RunTimeoutSec с"
        break
    }
    Start-Sleep 3
}
Shot 'guest_after.png'

if ($done) {
    Copy-Item (Join-Path $done.DirectoryName '*') $script:resDir -Force
    $rc = (Get-Content (Join-Path $script:resDir 'exitcode.txt') -ErrorAction SilentlyContinue | Select-Object -First 1)
    Log ("Готово за {0:N0} с, код возврата screens: {1}" -f ((Get-Date) - $t0).TotalSeconds, $rc)
}

if (-not $Keep) {
    Log 'Выключение ВМ'
    VB controlvm $Vm poweroff | Out-Null
}
Log "Результаты: $script:resDir"
