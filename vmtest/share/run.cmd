@echo off
rem Запускается в госте (Win+R \\vboxsrv\screens\run.cmd). Копирует screens_*.exe
rem в локальную папку, запускает с ключами из args.txt, результаты кладёт в
rem \\vboxsrv\screens\results\%COMPUTERNAME%\ ; done.txt пишется последним.
setlocal
set "SH=\\vboxsrv\screens"
set "W=%SystemDrive%\scr_test"
set "ARCH=%PROCESSOR_ARCHITECTURE%"
if defined PROCESSOR_ARCHITEW6432 set "ARCH=%PROCESSOR_ARCHITEW6432%"
set "EXE=screens_x86.exe"
if /i "%ARCH%"=="AMD64" set "EXE=screens_x64.exe"
set "ARGS="
if exist "%SH%\args.txt" set /p ARGS=<"%SH%\args.txt"
set "R=%SH%\results\%COMPUTERNAME%"

rem started.txt - хост видит, что запуск состоялся, и не повторяет Win+R
if not exist "%R%" mkdir "%R%"
> "%R%\started.txt" echo started
if exist "%W%" rd /s /q "%W%"
mkdir "%W%"
mkdir "%W%\out"
copy /y "%SH%\%EXE%" "%W%\" >nul

rem подготовка тестовых данных (vm_test.ps1 -Prep)
if exist "%SH%\prep.cmd" call "%SH%\prep.cmd" > "%R%\_prep.txt" 2>&1

rem defaultdir.txt (vm_test.ps1 -DefaultDir) - без папки результатов: утилита сама создаёт
rem <ИМЯ_ПК>_<IP> рядом с exe; её содержимое и имя (_folder.txt) переносятся в out
if exist "%SH%\defaultdir.txt" goto :run_default
echo run: %EXE% "%W%\out" %ARGS%
"%W%\%EXE%" "%W%\out" %ARGS% > "%W%\out\_stdout.txt" 2>&1
set "RC=%ERRORLEVEL%"
goto :results

:run_default
echo run: %EXE% %ARGS%
"%W%\%EXE%" %ARGS% > "%W%\out\_stdout.txt" 2>&1
set "RC=%ERRORLEVEL%"
for /d %%D in ("%W%\*_*") do (
    copy /y "%%D\*.*" "%W%\out\" >nul
    >> "%W%\out\_folder.txt" echo %%~nxD
)

:results
echo exit code: %RC%

copy /y "%W%\out\*.*" "%R%\" >nul
rem перенаправление спереди: "echo 0>файл" cmd понял бы как поток 0
> "%R%\exitcode.txt" echo %RC%
> "%R%\done.txt" echo done
endlocal
exit
