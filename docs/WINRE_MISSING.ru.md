# Инструкция: в Windows нет winre.wim (урезанная сборка)

Многие урезанные сборки Windows удаляют среду восстановления, чтобы уменьшить размер: нет `winre.wim`, а `reagentc /info` пишет, что WinRE отключена. Есть два пути.

## Путь 1 (проще всего): основа «Установочный образ Windows», `winre.wim` и ADK не нужны

Скачайте ISO Windows с сайта Microsoft (той же разрядности; лучше той же версии) или возьмите установочную флешку. В WinRepair откройте создание флешки WinPE, выберите основу **«Установочный образ Windows»** и укажите ISO (или букву диска). Программа сама возьмёт чистый WinPE из `sources\boot.wim`, добавит WinRepair и `rstrui`. WinRepair стартует первым. Подробности: `WINPE_USB.ru.md`. Если ADK уже установлен, можно выбрать основу ADK WinPE.

## Путь 2: достать winre.wim из образа Windows и вернуть в систему

Подходит, если нужен именно WinRE этой системы (запись в меню загрузки, WinRE + WinRepair).

### Шаг 1. Скачайте образ той же Windows
- Нужна **та же редакция, разрядность и по возможности та же версия сборки** (x64 с x64, ARM64 с ARM64). Версию вашей системы покажет `winver`.
- Официальный образ ISO: сайт Microsoft «Скачать Windows 10/11» (Media Creation Tool или образ ISO).
- Подключите ISO (правый клик → **Подключить**), он получит букву, например `E:`. Внутри есть `E:\sources\install.wim` или `E:\sources\install.esd`.

### Шаг 2. Достаньте winre.wim

**Вариант А: 7-Zip (проще, без монтирования).**
1. Установите 7-Zip (7-zip.org).
2. Откройте `E:\sources\install.wim` (или `.esd`) в 7-Zip.
3. Зайдите в папку с номером редакции (например `1`, `6`) → `Windows` → `System32` → `Recovery`.
4. Скопируйте `Winre.wim` на рабочий стол или в `C:\Temp`. Если папки `Recovery` нет, выберите другую редакцию (папку другого номера).

**Вариант Б: DISM (командная строка от администратора).**
```
dism /Get-WimInfo /WimFile:E:\sources\install.wim
```
Запомните номер нужной редакции, например `1`. Для `install.esd` используйте ту же команду с `.esd`.
```
mkdir C:\mnt
dism /Mount-Image /ImageFile:E:\sources\install.wim /Index:1 /MountDir:C:\mnt /ReadOnly
copy /Y C:\mnt\Windows\System32\Recovery\Winre.wim C:\Temp\
dism /Unmount-Image /MountDir:C:\mnt /Discard
```
Если DISM отказывает в чтении (ошибка 0x780 и подобные), используйте вариант А.

### Шаг 3. Положите файл на место

Файл должен лежать в `C:\Windows\System32\Recovery\` (папку создайте, если её нет). Командная строка от администратора:
```
mkdir C:\Windows\System32\Recovery
copy /Y C:\Temp\Winre.wim C:\Windows\System32\Recovery\Winre.wim
attrib +h +s C:\Windows\System32\Recovery\Winre.wim
```
Если Windows отказывает в копировании, возьмите папку во владение:
```
takeown /f C:\Windows\System32\Recovery /r /d y
icacls C:\Windows\System32\Recovery /grant *S-1-5-32-544:F /t
```

### Шаг 4. Зарегистрируйте WinRE в системе
```
reagentc /setreimage /path C:\Windows\System32\Recovery /target C:\Windows
reagentc /enable
reagentc /info
```
В выводе `reagentc /info` должно быть **Windows RE status: Enabled** и путь к `Winre.wim`. Если `reagentc /enable` пишет ошибку, то в вашей сборке вырезана сама подсистема восстановления; тогда используйте **Путь 1**.

### Шаг 5. Запустите WinRepair
Запустите программу от имени администратора. Теперь функции WinRE (запись в меню загрузки, WinRE + WinRepair, флешка на основе `winre.wim`) увидят образ. Для флешки всё равно рекомендуется основа ADK WinPE, потому что только на ней WinRepair гарантированно запускается первым.

## Что учесть
- Не берите `Winre.wim` от другой разрядности (x64 ≠ ARM64).
- Версия сборки образа лучше совпадает с вашей Windows. Разница в одну-две сборки обычно допустима, но на сильно разных версиях возможны проблемы с драйверами.
- Скачанные у посторонних образы WinRE не используйте: берите только официальные.
