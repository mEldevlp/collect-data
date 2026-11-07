# Этап 7: перенос сбора данных в программу

Что делают `collect_audit.cmd` и `collect_network.cmd` (родительская папка, Windows-1251, CRLF), как это переносится в программу и в каком порядке. Инвентаризация — 07.11.2026, решения приняты пользователем 07.11.2026 (варианты по умолчанию). Ход работ — в `CLAUDE.md`, раздел 10.

## 1. Решения

1. **Совместимость с `make_md.ps1` и `make_report_ib.cmd`** сохраняется: те же имена файлов и папок, заголовки блоков `[…]`, тексты итоговых файлов, форматы вывода (reg query, таблицы, sc qc, net …). Скрипты отчётов не переносятся и не меняются. Проверка — сверка отчётов make_md (`vmtest\compare_collect.ps1 -Report`).
2. **Кодировки**: все txt/csv — UTF-8 с BOM, CRLF, сразу при записи (перекодировки в конце нет, на XP без PowerShell тоже UTF-8). Вывод утилит декодируется при захвате: BOM UTF-16/UTF-8 → как есть, правильный UTF-8 → как есть, иначе построчно 866/1251 по оценке строки (`Get-Score` скрипта). Имена — только русские (`set_names_en` не нужен). В `выгрузку` — только сырой вывод, который программа разбирает (экспорты и журнал secedit, auditpol, net accounts, net user).
3. **Данные, прочитанные через WinAPI**, пишутся в формате заменяемой команды (reg query; sc qc с английскими подписями; таблицы Win32_* со строкой дефисов), заголовок блока тот же; в `_команды_сбора.txt` — равнозначная команда с пометкой «(WinAPI)».
4. **Папка** — как `VIEWER1_192.168.16.67`: `00_…`–`08_…`, `выгрузка`, `<ИМЯ>_<IP>_сетевые_связи.txt`, журналы, PNG в корне. Запасная — `%SystemDrive%\<ИМЯ>_<IP>` (как у программы сейчас). Повторный запуск: прежние результаты сбора (00–08, выгрузка, txt/csv корня, кроме журналов) удаляются, PNG перезаписываются, журналы дописываются. Скрипт удаляет все `*.txt` корня — программа так не делает (стёр бы журнал снимков).
5. **Журналы** — два: `_журнал_сбора.txt` (сбор; сообщения по-русски, последняя строка оканчивается на ` Finished` — по ней make_md считает сбор завершённым) и `_журнал_скриншотов.txt` (как сейчас).
6. **Порядок** — сбор → снимки в одном запуске. Снимать нельзя — сбор всё равно выполняется, код 2.
7. **Ключи**: `/ping`, `/nonet`, `/nopause` — как в скрипте; `/noscreens`, `/nocollect`; `/only:` принимает и разделы сбора (`sysinfo, software, time, audit, accounts, users, services, remote, logs, network`), `/list` — оба списка; `/timeout` — общий лимит, по умолчанию 1200 с; у каждой утилиты свой лимит (120 с, `net accounts /domain` — 30 с).
8. **Без прав администратора** — предупреждение, собрать доступное, в файлах пометка «нужны права администратора», код 1; пауза с советом перезапуститься — только при запуске из Проводника; самоповышения нет.
9. **Разрядность**: x86-сборка в WOW64 перезапускает `screens_x64.exe` из своей папки; нет его — работает сама с `KEY_WOW64_64KEY` и `Wow64DisableWow64FsRedirection`, «Внимание» в журнал.
10. **WMI**: служба Winmgmt не запущена — WMI не трогать (подключение её запустило бы), взять что можно через API, пометка в журнале. Команды `net` — только при запущенной службе и с «N» на вход.
11. **XP SP2**: проверить в части 3, читаются ли действующие политика аудита и права через LSA API (`LsaQueryInformationPolicy(PolicyAuditEventsInformation)`, `LsaEnumerateAccountsWithUserRight`), пароли/блокировка — `NetUserModalsGet`. Работает — брать оттуда, шаблон `secsetup.inf` оставить справочно.
12. Имя exe и cmd-скрипты не трогаем; `*_astra.sh` — тоже.

## 2. collect_audit.cmd по разделам

| Раздел | Что собирает, чем | Файлы | XP / 7 / 10, запасные варианты |
|---|---|---|---|
| начало | `Sysnative\cmd.exe` (64 бит), `chcp`/OEMCP, `ver`, права (`net session` с «N», `fsutil dirty`), wmic/PS, IP (`route print 0.0.0.0`/`ipconfig`), DomainRole (wmic/PS) | `_журнал_сбора.txt`, `_команды_сбора.txt` | удаляет прежние `*.txt` корня и папки 00–08 |
| `:prepare_secedit` | `secedit /export [/mergedpolicy] [/db] /cfg … /areas SECURITYPOLICY USER_RIGHTS [/quiet] /log`; `repair\secsetup.inf` до `[Registry Keys]` | `00_экспорт_политики_безопасности[_заданное_политиками].txt`, `00_журнал_экспорта_secedit.txt`, `00_шаблон_безопасности_при_установке.txt` | нет `/quiet` → без него; пусто → `/db`, без `/areas`; XP SP2 пусто всегда → шаблон |
| `:collect_sysinfo` | переменные (`Collection date:` … `Processor architecture:` — английские подписи, их читает make_md), `ver`, WMI OS/ComputerSystem, `systeminfo`, `ipconfig /all` | `00_сведения_о_системе.txt` | WMI: wmic → PS → «not available» |
| `:collect_software` | `reg query` подразделов Uninstall (HKLM, Wow6432Node, загруженные HKU\S-1-5-21-*), тип записи (program/update/system/nouninstall), `sort`; PS `SHLoadIndirectString`; PS `Shell.Application` — список окна | `01_установленные_программы.txt`, `01_список_из_окна_программы_и_компоненты.txt` | окно — 6.x с PS; reg на XP пишет ANSI; ®/™ → `?` |
| обновления | WMI QFE; XP — ещё `reg query …\Updates /s` (KB/Q) | `01_установленные_обновления.txt` | |
| `:collect_features` | 6.x WMI `Win32_OptionalFeature`, серверы — `Win32_ServerFeature`; XP — реестр `OC Manager\Subcomponents` | `01_компоненты_Windows.txt` | |
| `:collect_time` | `w32tm /tz`, 6.x `w32tm /query /status /source /configuration /peers`, XP `net time /querysntp`; sc W32Time; реестр W32Time, политики, часовой пояс; итог по реестру + RUNNING + `/source` | `02_подробные_настройки_службы_времени.txt`, `02_итог_синхронизации_времени.txt` | XP — итог только по реестру |
| `:collect_audit` | `[Event Audit]` из экспортов (политика → база → шаблон), реестр Lsa; 6.x `auditpol /get /category:*` (перевыравнивание PS), `/option:` ×4, `/resourceSACL` | `03_базовая_политика_аудита.txt`, `03_расширенная_политика_аудита.txt` | XP — расширенной нет |
| `:audit_events` | реестр Eventlog (3 журнала) и политики, 6.x `wevtutil gl` | `03_параметры_журналов_событий.txt` | |
| `:collect_accounts` | `net accounts` (+`/domain`), строки 2–5 и 6–8; значения secedit | `04_политика_паролей.txt`, `04_политика_блокировки_учетных_записей.txt`, `04_политика_Kerberos.txt` (КД) | XP SP2 — из шаблона |
| `:collect_users` | `net user` (колонки по 25), WMI `Win32_UserAccount`, `net user "имя"`, `net localgroup` (+ по группам), CSV из строк 2, 3, 6 net user | `05_список_учетных_записей.txt`, `05_вывод_net_user\ИМЯ_уз.txt`, `05_группы_и_их_состав.txt`, `05_сопоставление_с_работниками.csv` | |
| `:collect_services` | WMI `Win32_Service` (или `sc query`), по каждой `sc qc имя 8192`, `sc sdshow` | `06_список_служб.txt`, `06_настройки_служб.txt` | ≈30 с на XP; XP x64: мусор sc после пустого DEPENDENCIES; на русских 7/10 в `sc qc` попадает только часть служб (подписи `sc query` у запущенных и остановленных разные, скрипт берёт подпись первой записи) |
| `:collect_remote` | реестр (TS, RDP-Tcp, RA, winreg, LanmanServer/Workstation, Lsa, MSV1_0, Policies\System, Ole, WinRM, Telnet, SNMP, Firewall); sc 7 служб; группы 544/555/562/580 по SID + `net localgroup`; `[Privilege Rights]`; `winrm.cmd get/enumerate`; `sshd_config`; `net share`; netsh firewall/advfirewall; `netstat -ano`; `tasklist /svc`; поиск средств удалённого доступа | девять `07_*.txt` | winrm.vbs — ANSI; netsh Win11 — UTF-8; ValidCommunities SNMP выгружаются (make_md их маскирует) |
| `:collect_logs` | `dir /a-d` *.log (WINDIR, Inf), `xcopy` + `move` | `08_список_файлов_log.txt`, `08_файлы_log\WINDIR[_Inf]` | |
| `:run_network`, `:save_raw`, `:convert_utf8` | collect_network.cmd; `_tmp` → `выгрузка`; PS/JScript → UTF-8 BOM, CSV | | без PS и WSH — 866 |

## 3. collect_network.cmd

Один файл `<ИМЯ>_<IP>_сетевые_связи.txt`, разделы `===== N. … =====` (make_md его не читает, он для сверки со схемой КИИ):
1. General — версия, имя/IP/ОС, права; WMI ComputerSystem, OperatingSystem списком.
2. Adapters — `ipconfig /all`; WMI NetworkAdapter, NetworkAdapterConfiguration, `root\wmi` MSNdis_MediaConnectStatus/LinkSpeed; PS Get-NetAdapter (8+); `netsh wlan show interfaces`; POTSModem; SERIALCOMM; PnPEntity (порты, модемы, PROFIBUS/SIMATIC/Hilscher).
3. Routing — `route print`; Tcpip\Parameters, PersistentRoutes; sc RemoteAccess, SharedAccess.
4. Connections — 5 × `netstat -ano` через 15 с (со 2-го без LISTENING и `*:*`), `arp -a` в начале и конце, `netstat -e`, `netsh interface ipv4 show neighbors`.
5. Processes — `tasklist /svc`, WMI Win32_Process.
6. Names — hosts, lmhosts без комментариев, `ipconfig /displaydns`, `nbtstat -n -c -r -S`, `net use`, `net config workstation`, `net session` (при запущенных службах).
7. Settings — W32Time, SNMP PermittedManagers/TrapConfiguration, Zabbix (службы → конфиг → Server, ServerActive, Hostname, ListenPort, ListenIP, SourceIP), ODBC без паролей, `netsh winhttp show proxy`/`proxycfg`, rasphone.pbk, загруженные HKU\S-1-5-21-*.
8. Ping sweep — только `/ping` (СОВ на объекте примут за сканирование).

В эталонах файл сетевых связей — со смешанными переводами строк (CRLF и LF).

## 4. WinAPI или утилита

**Напрямую через WinAPI**: весь реестр (вывод в формате reg query); службы (SCM API — ADVAPI32: `EnumServicesStatusExW`, `QueryServiceConfigW`, `QueryServiceObjectSecurity`); список учётных записей и групп (`NetUserEnum`, `NetLocalGroupEnum`), `Win32_UserAccount`, имена групп по SID, CSV; файлы (hosts, lmhosts, sshd_config, rasphone.pbk, конфиг Zabbix, secsetup.inf, экспорты secedit, *.log — `CopyFileW`); сведения о системе, роль в домене (`DsRoleGetPrimaryDomainInformation`); WMI через COM (`IWbemLocator`, ole32/oleaut32 через LoadLibrary) вместо wmic/PowerShell; Win32_Process (Toolhelp); `/ping` (`GetIpForwardTable`, `IcmpSendEcho`, `GetIpNetTable`); логика (разбор INF, итог времени, перевыравнивание auditpol, фильтр netstat, поиск средств удалённого доступа).

**Утилитой с захватом вывода**: secedit, systeminfo, ver, ipconfig, w32tm, net time, auditpol, wevtutil, net accounts/user/localgroup/share/use/config/session, winrm, netsh, netstat, arp, route print, tasklist, nbtstat. Запуск: `CreateProcessW` по полному пути в System32, stdout+stderr в файл, на вход NUL или «N», job-объект (дерево процессов), таймаут на команду, сторож → `TerminateJobObject`, декодирование в UTF-8 с BOM.

**От чего зависят make_md.ps1 и make_report_ib.cmd**: папка распознаётся по `_журнал_сбора.txt`, «сбор завершён» — строка с окончанием ` Finished`; подписи `Collection date:` … `Administrator rights: yes|no` в `00_сведения`; заголовки блоков (`systeminfo`, `ipconfig /all`, `Win32_OperatingSystem`, `Win32_ComputerSystem`, `Win32_Service`, `sc query*`, `sc qc <имя>`, `Win32_UserAccount*`, `Win32_OptionalFeature*`, `Win32_ServerFeature`, `reg query*OC Manager*`, `Programs and Features`, `Installed updates`, `auditpol /get /option`, `Global object access*`, `wevtutil gl <журнал>`, `winrm …`, `*sshd_config`, `net share`, `netsh …`, `netstat -ano`, `tasklist /svc`); форматы внутри (reg query, таблица wmic или заголовок + дефисы, порядок полей sc qc, «подпись  значение» net, строки net accounts с двоеточием, строки итогов `  описание: значение; Имя = N`); пути файлов (RU и EN) у make_report_ib; снимок ПО — префикс `Установленное ПО`.

## 5. Части

Каждая часть: серия на 6 ВМ с `-DefaultDir`, сверка с эталоном (`make_summary.ps1 -Collect`), обновление CLAUDE.md (разделы 3/8/10), итог `vmtest\итог_этап7_<часть>`.

0. Эталон и сверка (без C) — `vmtest\compare_collect.ps1`, режим `-Script` у `vm_test.ps1`/`vm_series.ps1`, копирование дерева папок в `run.cmd`. Готово 07.11.2026.
1. Каркас: ключи, папка, журналы, запуск утилит (`exec.c`, `text.c`), файлы с блоками `[…]` (`outfile.c`), таблица разделов (`collect.c`), порядок «сбор → снимки», сторож; разделы 00 (без WMI) и 08. Вывод реестра в формате reg query перенесён в часть 2 (в 00 и 08 реестра нет — проверить было бы не на чем), разрядность (решение 9) — в часть 7.
2. WMI через COM + WMI-части 00 + раздел 01; вывод реестра в формате reg query (`regdump`).
3. secedit и разбор INF + 03, 04, 02; проверка LSA на XP SP2.
4. 05 и 06.
5. 07.
6. Сетевые связи.
7. Полный прогон «сбор → снимки», без прав администратора, разрядность (x86 в WOW64 перезапускает x64), сверка отчётов make_md/make_report_ib, таймауты.
