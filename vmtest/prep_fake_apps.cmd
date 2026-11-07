@echo off
rem Тестовые данные для постраничной съёмки списка ПО: 50 фиктивных программ в реестре.
rem Только для тестовых ВМ (снимок clean откатывается после прогона). Имена латиницей:
rem cmd читает .cmd в OEM-кодировке, кириллица в реестр записалась бы неправильно.
set "K=HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall"
reg add "%K%\ScreensTest01" /v DisplayName /d "Screens Test Program 01" /f >nul
reg add "%K%\ScreensTest01" /v UninstallString /d "cmd.exe /c echo test" /f >nul
reg add "%K%\ScreensTest01" /v Publisher /d "Screens Test Ltd" /f >nul
reg add "%K%\ScreensTest01" /v DisplayVersion /d "1.0.01" /f >nul
reg add "%K%\ScreensTest02" /v DisplayName /d "Screens Test Program 02" /f >nul
reg add "%K%\ScreensTest02" /v UninstallString /d "cmd.exe /c echo test" /f >nul
reg add "%K%\ScreensTest02" /v Publisher /d "Screens Test Ltd" /f >nul
reg add "%K%\ScreensTest02" /v DisplayVersion /d "1.0.02" /f >nul
reg add "%K%\ScreensTest03" /v DisplayName /d "Screens Test Program 03" /f >nul
reg add "%K%\ScreensTest03" /v UninstallString /d "cmd.exe /c echo test" /f >nul
reg add "%K%\ScreensTest03" /v Publisher /d "Screens Test Ltd" /f >nul
reg add "%K%\ScreensTest03" /v DisplayVersion /d "1.0.03" /f >nul
reg add "%K%\ScreensTest04" /v DisplayName /d "Screens Test Program 04" /f >nul
reg add "%K%\ScreensTest04" /v UninstallString /d "cmd.exe /c echo test" /f >nul
reg add "%K%\ScreensTest04" /v Publisher /d "Screens Test Ltd" /f >nul
reg add "%K%\ScreensTest04" /v DisplayVersion /d "1.0.04" /f >nul
reg add "%K%\ScreensTest05" /v DisplayName /d "Screens Test Program 05" /f >nul
reg add "%K%\ScreensTest05" /v UninstallString /d "cmd.exe /c echo test" /f >nul
reg add "%K%\ScreensTest05" /v Publisher /d "Screens Test Ltd" /f >nul
reg add "%K%\ScreensTest05" /v DisplayVersion /d "1.0.05" /f >nul
reg add "%K%\ScreensTest06" /v DisplayName /d "Screens Test Program 06" /f >nul
reg add "%K%\ScreensTest06" /v UninstallString /d "cmd.exe /c echo test" /f >nul
reg add "%K%\ScreensTest06" /v Publisher /d "Screens Test Ltd" /f >nul
reg add "%K%\ScreensTest06" /v DisplayVersion /d "1.0.06" /f >nul
reg add "%K%\ScreensTest07" /v DisplayName /d "Screens Test Program 07" /f >nul
reg add "%K%\ScreensTest07" /v UninstallString /d "cmd.exe /c echo test" /f >nul
reg add "%K%\ScreensTest07" /v Publisher /d "Screens Test Ltd" /f >nul
reg add "%K%\ScreensTest07" /v DisplayVersion /d "1.0.07" /f >nul
reg add "%K%\ScreensTest08" /v DisplayName /d "Screens Test Program 08" /f >nul
reg add "%K%\ScreensTest08" /v UninstallString /d "cmd.exe /c echo test" /f >nul
reg add "%K%\ScreensTest08" /v Publisher /d "Screens Test Ltd" /f >nul
reg add "%K%\ScreensTest08" /v DisplayVersion /d "1.0.08" /f >nul
reg add "%K%\ScreensTest09" /v DisplayName /d "Screens Test Program 09" /f >nul
reg add "%K%\ScreensTest09" /v UninstallString /d "cmd.exe /c echo test" /f >nul
reg add "%K%\ScreensTest09" /v Publisher /d "Screens Test Ltd" /f >nul
reg add "%K%\ScreensTest09" /v DisplayVersion /d "1.0.09" /f >nul
reg add "%K%\ScreensTest10" /v DisplayName /d "Screens Test Program 10" /f >nul
reg add "%K%\ScreensTest10" /v UninstallString /d "cmd.exe /c echo test" /f >nul
reg add "%K%\ScreensTest10" /v Publisher /d "Screens Test Ltd" /f >nul
reg add "%K%\ScreensTest10" /v DisplayVersion /d "1.0.10" /f >nul
reg add "%K%\ScreensTest11" /v DisplayName /d "Screens Test Program 11" /f >nul
reg add "%K%\ScreensTest11" /v UninstallString /d "cmd.exe /c echo test" /f >nul
reg add "%K%\ScreensTest11" /v Publisher /d "Screens Test Ltd" /f >nul
reg add "%K%\ScreensTest11" /v DisplayVersion /d "1.0.11" /f >nul
reg add "%K%\ScreensTest12" /v DisplayName /d "Screens Test Program 12" /f >nul
reg add "%K%\ScreensTest12" /v UninstallString /d "cmd.exe /c echo test" /f >nul
reg add "%K%\ScreensTest12" /v Publisher /d "Screens Test Ltd" /f >nul
reg add "%K%\ScreensTest12" /v DisplayVersion /d "1.0.12" /f >nul
reg add "%K%\ScreensTest13" /v DisplayName /d "Screens Test Program 13" /f >nul
reg add "%K%\ScreensTest13" /v UninstallString /d "cmd.exe /c echo test" /f >nul
reg add "%K%\ScreensTest13" /v Publisher /d "Screens Test Ltd" /f >nul
reg add "%K%\ScreensTest13" /v DisplayVersion /d "1.0.13" /f >nul
reg add "%K%\ScreensTest14" /v DisplayName /d "Screens Test Program 14" /f >nul
reg add "%K%\ScreensTest14" /v UninstallString /d "cmd.exe /c echo test" /f >nul
reg add "%K%\ScreensTest14" /v Publisher /d "Screens Test Ltd" /f >nul
reg add "%K%\ScreensTest14" /v DisplayVersion /d "1.0.14" /f >nul
reg add "%K%\ScreensTest15" /v DisplayName /d "Screens Test Program 15" /f >nul
reg add "%K%\ScreensTest15" /v UninstallString /d "cmd.exe /c echo test" /f >nul
reg add "%K%\ScreensTest15" /v Publisher /d "Screens Test Ltd" /f >nul
reg add "%K%\ScreensTest15" /v DisplayVersion /d "1.0.15" /f >nul
reg add "%K%\ScreensTest16" /v DisplayName /d "Screens Test Program 16" /f >nul
reg add "%K%\ScreensTest16" /v UninstallString /d "cmd.exe /c echo test" /f >nul
reg add "%K%\ScreensTest16" /v Publisher /d "Screens Test Ltd" /f >nul
reg add "%K%\ScreensTest16" /v DisplayVersion /d "1.0.16" /f >nul
reg add "%K%\ScreensTest17" /v DisplayName /d "Screens Test Program 17" /f >nul
reg add "%K%\ScreensTest17" /v UninstallString /d "cmd.exe /c echo test" /f >nul
reg add "%K%\ScreensTest17" /v Publisher /d "Screens Test Ltd" /f >nul
reg add "%K%\ScreensTest17" /v DisplayVersion /d "1.0.17" /f >nul
reg add "%K%\ScreensTest18" /v DisplayName /d "Screens Test Program 18" /f >nul
reg add "%K%\ScreensTest18" /v UninstallString /d "cmd.exe /c echo test" /f >nul
reg add "%K%\ScreensTest18" /v Publisher /d "Screens Test Ltd" /f >nul
reg add "%K%\ScreensTest18" /v DisplayVersion /d "1.0.18" /f >nul
reg add "%K%\ScreensTest19" /v DisplayName /d "Screens Test Program 19" /f >nul
reg add "%K%\ScreensTest19" /v UninstallString /d "cmd.exe /c echo test" /f >nul
reg add "%K%\ScreensTest19" /v Publisher /d "Screens Test Ltd" /f >nul
reg add "%K%\ScreensTest19" /v DisplayVersion /d "1.0.19" /f >nul
reg add "%K%\ScreensTest20" /v DisplayName /d "Screens Test Program 20" /f >nul
reg add "%K%\ScreensTest20" /v UninstallString /d "cmd.exe /c echo test" /f >nul
reg add "%K%\ScreensTest20" /v Publisher /d "Screens Test Ltd" /f >nul
reg add "%K%\ScreensTest20" /v DisplayVersion /d "1.0.20" /f >nul
reg add "%K%\ScreensTest21" /v DisplayName /d "Screens Test Program 21" /f >nul
reg add "%K%\ScreensTest21" /v UninstallString /d "cmd.exe /c echo test" /f >nul
reg add "%K%\ScreensTest21" /v Publisher /d "Screens Test Ltd" /f >nul
reg add "%K%\ScreensTest21" /v DisplayVersion /d "1.0.21" /f >nul
reg add "%K%\ScreensTest22" /v DisplayName /d "Screens Test Program 22" /f >nul
reg add "%K%\ScreensTest22" /v UninstallString /d "cmd.exe /c echo test" /f >nul
reg add "%K%\ScreensTest22" /v Publisher /d "Screens Test Ltd" /f >nul
reg add "%K%\ScreensTest22" /v DisplayVersion /d "1.0.22" /f >nul
reg add "%K%\ScreensTest23" /v DisplayName /d "Screens Test Program 23" /f >nul
reg add "%K%\ScreensTest23" /v UninstallString /d "cmd.exe /c echo test" /f >nul
reg add "%K%\ScreensTest23" /v Publisher /d "Screens Test Ltd" /f >nul
reg add "%K%\ScreensTest23" /v DisplayVersion /d "1.0.23" /f >nul
reg add "%K%\ScreensTest24" /v DisplayName /d "Screens Test Program 24" /f >nul
reg add "%K%\ScreensTest24" /v UninstallString /d "cmd.exe /c echo test" /f >nul
reg add "%K%\ScreensTest24" /v Publisher /d "Screens Test Ltd" /f >nul
reg add "%K%\ScreensTest24" /v DisplayVersion /d "1.0.24" /f >nul
reg add "%K%\ScreensTest25" /v DisplayName /d "Screens Test Program 25" /f >nul
reg add "%K%\ScreensTest25" /v UninstallString /d "cmd.exe /c echo test" /f >nul
reg add "%K%\ScreensTest25" /v Publisher /d "Screens Test Ltd" /f >nul
reg add "%K%\ScreensTest25" /v DisplayVersion /d "1.0.25" /f >nul
reg add "%K%\ScreensTest26" /v DisplayName /d "Screens Test Program 26" /f >nul
reg add "%K%\ScreensTest26" /v UninstallString /d "cmd.exe /c echo test" /f >nul
reg add "%K%\ScreensTest26" /v Publisher /d "Screens Test Ltd" /f >nul
reg add "%K%\ScreensTest26" /v DisplayVersion /d "1.0.26" /f >nul
reg add "%K%\ScreensTest27" /v DisplayName /d "Screens Test Program 27" /f >nul
reg add "%K%\ScreensTest27" /v UninstallString /d "cmd.exe /c echo test" /f >nul
reg add "%K%\ScreensTest27" /v Publisher /d "Screens Test Ltd" /f >nul
reg add "%K%\ScreensTest27" /v DisplayVersion /d "1.0.27" /f >nul
reg add "%K%\ScreensTest28" /v DisplayName /d "Screens Test Program 28" /f >nul
reg add "%K%\ScreensTest28" /v UninstallString /d "cmd.exe /c echo test" /f >nul
reg add "%K%\ScreensTest28" /v Publisher /d "Screens Test Ltd" /f >nul
reg add "%K%\ScreensTest28" /v DisplayVersion /d "1.0.28" /f >nul
reg add "%K%\ScreensTest29" /v DisplayName /d "Screens Test Program 29" /f >nul
reg add "%K%\ScreensTest29" /v UninstallString /d "cmd.exe /c echo test" /f >nul
reg add "%K%\ScreensTest29" /v Publisher /d "Screens Test Ltd" /f >nul
reg add "%K%\ScreensTest29" /v DisplayVersion /d "1.0.29" /f >nul
reg add "%K%\ScreensTest30" /v DisplayName /d "Screens Test Program 30" /f >nul
reg add "%K%\ScreensTest30" /v UninstallString /d "cmd.exe /c echo test" /f >nul
reg add "%K%\ScreensTest30" /v Publisher /d "Screens Test Ltd" /f >nul
reg add "%K%\ScreensTest30" /v DisplayVersion /d "1.0.30" /f >nul
reg add "%K%\ScreensTest31" /v DisplayName /d "Screens Test Program 31" /f >nul
reg add "%K%\ScreensTest31" /v UninstallString /d "cmd.exe /c echo test" /f >nul
reg add "%K%\ScreensTest31" /v Publisher /d "Screens Test Ltd" /f >nul
reg add "%K%\ScreensTest31" /v DisplayVersion /d "1.0.31" /f >nul
reg add "%K%\ScreensTest32" /v DisplayName /d "Screens Test Program 32" /f >nul
reg add "%K%\ScreensTest32" /v UninstallString /d "cmd.exe /c echo test" /f >nul
reg add "%K%\ScreensTest32" /v Publisher /d "Screens Test Ltd" /f >nul
reg add "%K%\ScreensTest32" /v DisplayVersion /d "1.0.32" /f >nul
reg add "%K%\ScreensTest33" /v DisplayName /d "Screens Test Program 33" /f >nul
reg add "%K%\ScreensTest33" /v UninstallString /d "cmd.exe /c echo test" /f >nul
reg add "%K%\ScreensTest33" /v Publisher /d "Screens Test Ltd" /f >nul
reg add "%K%\ScreensTest33" /v DisplayVersion /d "1.0.33" /f >nul
reg add "%K%\ScreensTest34" /v DisplayName /d "Screens Test Program 34" /f >nul
reg add "%K%\ScreensTest34" /v UninstallString /d "cmd.exe /c echo test" /f >nul
reg add "%K%\ScreensTest34" /v Publisher /d "Screens Test Ltd" /f >nul
reg add "%K%\ScreensTest34" /v DisplayVersion /d "1.0.34" /f >nul
reg add "%K%\ScreensTest35" /v DisplayName /d "Screens Test Program 35" /f >nul
reg add "%K%\ScreensTest35" /v UninstallString /d "cmd.exe /c echo test" /f >nul
reg add "%K%\ScreensTest35" /v Publisher /d "Screens Test Ltd" /f >nul
reg add "%K%\ScreensTest35" /v DisplayVersion /d "1.0.35" /f >nul
reg add "%K%\ScreensTest36" /v DisplayName /d "Screens Test Program 36" /f >nul
reg add "%K%\ScreensTest36" /v UninstallString /d "cmd.exe /c echo test" /f >nul
reg add "%K%\ScreensTest36" /v Publisher /d "Screens Test Ltd" /f >nul
reg add "%K%\ScreensTest36" /v DisplayVersion /d "1.0.36" /f >nul
reg add "%K%\ScreensTest37" /v DisplayName /d "Screens Test Program 37" /f >nul
reg add "%K%\ScreensTest37" /v UninstallString /d "cmd.exe /c echo test" /f >nul
reg add "%K%\ScreensTest37" /v Publisher /d "Screens Test Ltd" /f >nul
reg add "%K%\ScreensTest37" /v DisplayVersion /d "1.0.37" /f >nul
reg add "%K%\ScreensTest38" /v DisplayName /d "Screens Test Program 38" /f >nul
reg add "%K%\ScreensTest38" /v UninstallString /d "cmd.exe /c echo test" /f >nul
reg add "%K%\ScreensTest38" /v Publisher /d "Screens Test Ltd" /f >nul
reg add "%K%\ScreensTest38" /v DisplayVersion /d "1.0.38" /f >nul
reg add "%K%\ScreensTest39" /v DisplayName /d "Screens Test Program 39" /f >nul
reg add "%K%\ScreensTest39" /v UninstallString /d "cmd.exe /c echo test" /f >nul
reg add "%K%\ScreensTest39" /v Publisher /d "Screens Test Ltd" /f >nul
reg add "%K%\ScreensTest39" /v DisplayVersion /d "1.0.39" /f >nul
reg add "%K%\ScreensTest40" /v DisplayName /d "Screens Test Program 40" /f >nul
reg add "%K%\ScreensTest40" /v UninstallString /d "cmd.exe /c echo test" /f >nul
reg add "%K%\ScreensTest40" /v Publisher /d "Screens Test Ltd" /f >nul
reg add "%K%\ScreensTest40" /v DisplayVersion /d "1.0.40" /f >nul
reg add "%K%\ScreensTest41" /v DisplayName /d "Screens Test Program 41" /f >nul
reg add "%K%\ScreensTest41" /v UninstallString /d "cmd.exe /c echo test" /f >nul
reg add "%K%\ScreensTest41" /v Publisher /d "Screens Test Ltd" /f >nul
reg add "%K%\ScreensTest41" /v DisplayVersion /d "1.0.41" /f >nul
reg add "%K%\ScreensTest42" /v DisplayName /d "Screens Test Program 42" /f >nul
reg add "%K%\ScreensTest42" /v UninstallString /d "cmd.exe /c echo test" /f >nul
reg add "%K%\ScreensTest42" /v Publisher /d "Screens Test Ltd" /f >nul
reg add "%K%\ScreensTest42" /v DisplayVersion /d "1.0.42" /f >nul
reg add "%K%\ScreensTest43" /v DisplayName /d "Screens Test Program 43" /f >nul
reg add "%K%\ScreensTest43" /v UninstallString /d "cmd.exe /c echo test" /f >nul
reg add "%K%\ScreensTest43" /v Publisher /d "Screens Test Ltd" /f >nul
reg add "%K%\ScreensTest43" /v DisplayVersion /d "1.0.43" /f >nul
reg add "%K%\ScreensTest44" /v DisplayName /d "Screens Test Program 44" /f >nul
reg add "%K%\ScreensTest44" /v UninstallString /d "cmd.exe /c echo test" /f >nul
reg add "%K%\ScreensTest44" /v Publisher /d "Screens Test Ltd" /f >nul
reg add "%K%\ScreensTest44" /v DisplayVersion /d "1.0.44" /f >nul
reg add "%K%\ScreensTest45" /v DisplayName /d "Screens Test Program 45" /f >nul
reg add "%K%\ScreensTest45" /v UninstallString /d "cmd.exe /c echo test" /f >nul
reg add "%K%\ScreensTest45" /v Publisher /d "Screens Test Ltd" /f >nul
reg add "%K%\ScreensTest45" /v DisplayVersion /d "1.0.45" /f >nul
reg add "%K%\ScreensTest46" /v DisplayName /d "Screens Test Program 46" /f >nul
reg add "%K%\ScreensTest46" /v UninstallString /d "cmd.exe /c echo test" /f >nul
reg add "%K%\ScreensTest46" /v Publisher /d "Screens Test Ltd" /f >nul
reg add "%K%\ScreensTest46" /v DisplayVersion /d "1.0.46" /f >nul
reg add "%K%\ScreensTest47" /v DisplayName /d "Screens Test Program 47" /f >nul
reg add "%K%\ScreensTest47" /v UninstallString /d "cmd.exe /c echo test" /f >nul
reg add "%K%\ScreensTest47" /v Publisher /d "Screens Test Ltd" /f >nul
reg add "%K%\ScreensTest47" /v DisplayVersion /d "1.0.47" /f >nul
reg add "%K%\ScreensTest48" /v DisplayName /d "Screens Test Program 48" /f >nul
reg add "%K%\ScreensTest48" /v UninstallString /d "cmd.exe /c echo test" /f >nul
reg add "%K%\ScreensTest48" /v Publisher /d "Screens Test Ltd" /f >nul
reg add "%K%\ScreensTest48" /v DisplayVersion /d "1.0.48" /f >nul
reg add "%K%\ScreensTest49" /v DisplayName /d "Screens Test Program 49" /f >nul
reg add "%K%\ScreensTest49" /v UninstallString /d "cmd.exe /c echo test" /f >nul
reg add "%K%\ScreensTest49" /v Publisher /d "Screens Test Ltd" /f >nul
reg add "%K%\ScreensTest49" /v DisplayVersion /d "1.0.49" /f >nul
reg add "%K%\ScreensTest50" /v DisplayName /d "Screens Test Program 50" /f >nul
reg add "%K%\ScreensTest50" /v UninstallString /d "cmd.exe /c echo test" /f >nul
reg add "%K%\ScreensTest50" /v Publisher /d "Screens Test Ltd" /f >nul
reg add "%K%\ScreensTest50" /v DisplayVersion /d "1.0.50" /f >nul
echo prep: 50 fake programs added
