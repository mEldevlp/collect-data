@echo off
chcp 1251 >nul
setlocal EnableExtensions EnableDelayedExpansion
rem ============================================================================
rem  Сборка screens_x86.exe и screens_x64.exe (Release, статический рантайм).
rem  Основной набор - v141_xp (VS 2017 + Windows 7.1A SDK): exe работают на XP.
rem  Ставится в Build Tools for Visual Studio 2022, компонент
rem  "C++ Windows XP Support for VS 2017 (v141) tools [Deprecated]".
rem
rem  build.cmd                 - сборка v141_xp в ..\bin\ + проверка заголовков
rem  build.cmd /toolset:v145   - пробная сборка другим набором в obj\ (НЕ для XP)
rem  build.cmd /nocheck        - без проверки dumpbin
rem
rem  Код возврата: 0 - собрано и проверено, 1 - ошибка сборки/проверки,
rem                2 - не найден Visual Studio / нужный набор.
rem ============================================================================

set "HERE=%~dp0"
cd /d "%HERE%"
set "TOOLSET=v141_xp"
set "CHECK=1"

:args
if "%~1"=="" goto args_done
set "A=%~1"
if /i "!A:~0,9!"=="/toolset:" (
    set "TOOLSET=!A:~9!"
) else if /i "!A!"=="/nocheck" (
    set "CHECK=0"
) else (
    echo Неизвестный ключ: %1
    exit /b 2
)
shift
goto args

:args_done
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo Не найден vswhere.exe - Visual Studio не установлена.
    exit /b 2
)

rem Для v141_xp нужна установка с компонентом поддержки XP (VS 2026 его не содержит)
set "REQ="
if /i "%TOOLSET%"=="v141_xp" set "REQ=-requires Microsoft.VisualStudio.Component.WinXP"

set "VSDIR="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -prerelease -products * %REQ% -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
    echo Не найдена установка Visual Studio с набором %TOOLSET%.
    if /i "%TOOLSET%"=="v141_xp" (
        echo Поставьте Build Tools for Visual Studio 2022 с компонентом
        echo "C++ Windows XP Support for VS 2017 (v141) tools [Deprecated]".
    )
    exit /b 2
)
set "MSBUILD=%VSDIR%\MSBuild\Current\Bin\MSBuild.exe"
if not exist "%MSBUILD%" (
    echo Не найден MSBuild: "!MSBUILD!"
    exit /b 2
)
echo Visual Studio: "%VSDIR%"
echo Набор:         %TOOLSET%

set "RC=0"
for %%P in (Win32 x64) do (
    echo.
    echo ==== Сборка %%P ====
    "%MSBUILD%" screens.vcxproj -nologo -v:minimal -m -p:Configuration=Release -p:Platform=%%P -p:PlatformToolset=%TOOLSET%
    if errorlevel 1 (
        echo ОШИБКА сборки %%P
        set "RC=1"
    )
)
if not "%RC%"=="0" exit /b %RC%

if /i "%TOOLSET%"=="v141_xp" (
    set "OUTDIR=%HERE%..\bin"
) else (
    set "OUTDIR=%HERE%obj\%TOOLSET%\Release_x86"
)

if "%CHECK%"=="0" goto done

rem ---- Проверка: версия подсистемы и импорты ----
set "DUMPBIN="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -prerelease -products * %REQ% -find VC\Tools\MSVC\**\bin\Hostx86\x86\dumpbin.exe`) do set "DUMPBIN=%%i"
if not defined DUMPBIN (
    echo Не найден dumpbin.exe, проверка пропущена.
    goto done
)

call :check "x86" "%OUTDIR%\screens_x86.exe" "5.01"
if /i "%TOOLSET%"=="v141_xp" (
    call :check "x64" "%OUTDIR%\screens_x64.exe" "5.02"
) else (
    call :check "x64" "%HERE%obj\%TOOLSET%\Release_x64\screens_x64.exe" "6.00"
)

:done
echo.
if "%RC%"=="0" (echo Готово.) else (echo ЕСТЬ ОШИБКИ.)
exit /b %RC%

rem ---- :check <разрядность> <exe> <нужная версия подсистемы> ----
:check
echo.
echo ==== Проверка %~1: %~2 ====
if not exist "%~2" (
    echo Нет файла "%~2"
    set "RC=1"
    goto :eof
)
set "SUBSYS="
set "TMPF=%TEMP%\screens_build_%RANDOM%.txt"
"%DUMPBIN%" /nologo /headers "%~2" > "%TMPF%"
for /f "usebackq tokens=1-4" %%a in ("%TMPF%") do (
    if /i "%%b %%c"=="subsystem version" set "SUBSYS=%%a"
    if /i "%%b %%c %%d"=="operating system version" echo   версия ОС в заголовке: %%a
)
echo   версия подсистемы: %SUBSYS% (нужна %~3)
if not "%SUBSYS%"=="%~3" (
    if /i "%TOOLSET%"=="v141_xp" (
        echo   ОШИБКА: XP не запустит exe с такой версией подсистемы
        set "RC=1"
    )
)
echo   Импорт:
"%DUMPBIN%" /nologo /imports "%~2" > "%TMPF%"
for /f "usebackq tokens=1" %%a in (`findstr /i /r /c:"^ *[a-z0-9_-]*\.dll$" "%TMPF%"`) do echo     %%a
del "%TMPF%" 2>nul
goto :eof