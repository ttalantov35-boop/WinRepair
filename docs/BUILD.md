# Сборка, размер и выпуск

## Сборка
* **MSVC** (рекомендуется): *x64 Native Tools Command Prompt* → `build_msvc.bat` → `build\WinRepair.exe`. Для x86 — *x86 Native Tools*.
  Флаги сборки «на размер»: `/O1 /Os /GL /Gy /Gw /Zc:inline /MT`, линковка `/LTCG /OPT:REF /OPT:ICF /INCREMENTAL:NO /DEBUG:NONE`. Статическая CRT: внешних Redistributable не нужно.
* **MinGW-w64**: `build_mingw.bat` (`-Os`, `-Wl,--gc-sections`, `-s`, статическая сборка).
* Проверка зависимостей: `dumpbin /dependents build\WinRepair.exe` — только системные DLL (kernel32, user32, advapi32, setupapi, iphlpapi, ws2_32, comctl32 …).

## Пакет для флешки
`make_package.bat x64` (из x64-консоли) и затем `make_package.bat x86` (из x86-консоли) собирают `dist\WinRepair\`:
`WinRepair_x64.exe`, `WinRepair_x86.exe`, `Run-WinRepair.cmd`, `Playbooks\`, `Tools\`, `README_RU.txt`, примеры автозапуска и `SHA256.txt`. Папку целиком копируйте в корень флешки (`\WinRepair\`).

## Размер
Цель — меньше 2 МБ на одну архитектуру. Статическая сборка без Qt/MFC/.NET обычно укладывается в 1–1,5 МБ; фактический размер смотрите в конце вывода `make_package.bat`.
Если exe окажется больше: (1) соберите без `/GL`-ошибок линковки (нужна пара `/GL` + `/LTCG`), (2) убедитесь, что собираете Release без отладочной информации (`/DEBUG:NONE`),
(3) не используйте упаковщики типа UPX — они дают ложные срабатывания антивирусов и ломают проверку целостности.

## Контроль качества перед выпуском
1. `./run_tests.sh` — тесты ядра и проверка всех `Playbooks\*.wrp`.
2. Сборка x64 и x86 без предупреждений уровня /W3.
3. Проверка на чистых Windows 10/11 (запрос UAC), в WinPE (Sergei Strelec) и в WinRE; сценарии на ВМ (UEFI и BIOS).
4. Подпись exe (при наличии сертификата): `signtool sign /fd SHA256 /tr <timestamp-url> /td SHA256 WinRepair_x64.exe`.
