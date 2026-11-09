# План работ и архитектура

## Этапы
1. Анализ исходной утилиты (15 функций модуля PowerShell, каталог BSOD) — выполнен.
2. Архитектура: независимое ядро (`src/core`, без Windows API, покрыто тестами) + слой Windows (`src/win`) + модули (`src/modules`) + UI (`src/main.cpp`) — выполнен.
3. Перенос функциональности на нативные API (таблица ниже) — выполнен.
4. GUI: вкладки по темам, фоновые потоки, отмена, живой лог, лог-файл — выполнен.
5. Добавлено сверх исходной утилиты: офлайн-режим WinPE (выбор целевой ОС), быстрый разбор заголовка дампа без cdb,
   CLI `/full`, preflight (место, ожидающая перезагрузка, батарея), подтверждение изменяющих действий, версия-ресурс и манифест.
6. Сборка/тесты на Windows 10/11 и WinPE (x64/x86) — **за вами**, чек-лист ниже.
7. Следующие итерации (предложение): загрузка офлайн-реестра (SOFTWARE/SYSTEM), поиск и монтирование ESP для BCD в WinPE,
   хостинг «Источник DISM» через выбор файла, подпись exe, локализация EN/RU, MSI/SFX-упаковка.

## Соответствие PowerShell → нативный код
| Было | Стало |
|---|---|
| Get-CimInstance Win32_BIOS/BaseBoard/PhysicalMemory | разбор SMBIOS (`GetSystemFirmwareTable('RSMB')`) |
| Get-Disk / Get-PhysicalDisk / StorageReliabilityCounter | `\\.\PhysicalDriveN`: IOCTL_STORAGE_QUERY_PROPERTY, NVMe log page 02h, ATA SMART, PREDICT_FAILURE |
| Get-Volume | GetVolumeInformation + FSCTL_IS_VOLUME_DIRTY |
| Get-PnpDevice -Status ERROR | SetupAPI + CM_Get_DevNode_Status |
| Get-WinEvent | wevtapi (EvtQuery; файл .evtx для офлайн) |
| Confirm-SecureBootUEFI / Get-Tpm | реестр SecureBoot\State / tbs.dll |
| Get-NetAdapter, Test-Connection, Resolve-DnsName, Test-NetConnection | iphlpapi, ICMP, getaddrinfo, TCP connect |
| Checkpoint-Computer | SRSetRestorePointW (srclient.dll) |
| DISM / sfc / chkdsk / bcdedit / reagentc / verifier | запуск системных утилит со скрытым окном и захватом вывода |
| ConvertTo-Json, HTML-отчёт | собственный генератор |

## Известные отличия от исходной утилиты
* SATA: «ошибки чтения» = pending + uncorrectable сектора (SMART 197/198); «износ» — по атрибутам 177/231/233 (эвристика). NVMe — штатные поля SMART.
* TPM: показывается наличие и версия (Ready/Enabled/Activated без WMI получить нельзя).
* Устройства в состоянии «отключено» (проблема 22) не снижают оценку здоровья.
* Приватные файлы модуля (Test-WRPreflight, Write-WRLog и др.) не были в архиве — preflight и лог реализованы заново.
* Каталог BSOD встроен в exe (с кодами BugCheck); подсказки переформулированы под кнопки программы.

## Чек-лист приёмки (на Windows)
- [ ] Сборка MSVC x64 и x86 без ошибок; `dumpbin /dependents` — только системные DLL.
- [ ] Запуск на чистой Windows 10 и 11: запрос UAC, все кнопки отрабатывают, лог пишется.
- [ ] WinPE 10/11 (x64): окно открывается, офлайн-цель находится, события читаются из .evtx, DISM/SFC /offline работают.
- [ ] «Полная диагностика»: 9 шагов, report.html, оценка совпадает с ожиданием на заведомо больной тестовой ВМ.
- [ ] Отмена посреди DISM убивает дочерние процессы.
- [ ] Ремонт и точка восстановления запрашивают подтверждение.
