# Сценарии (playbooks) WinRepair

Сценарии — текстовые файлы `*.wrp` (UTF-8). Программа читает их при запуске из папки **`Playbooks\` рядом с exe** и из **`Data\Playbooks\`**
(второй каталог — для ваших сценариев; сценарий с тем же `id` заменяет поставляемый). Перекомпиляция не нужна: кнопка «Перезагрузить список» в окне «Сценарии ремонта».
Проверка файлов на ошибки: `./run_tests.sh` (или смотрите «ОШИБКИ РАЗБОРА» в окне сценариев и в журнале).

## Два вида сценариев
* **`kind = repair`** — пошаговая процедура ремонта (окно «Сценарии ремонта»). Каждый шаг, изменяющий систему, подтверждается отдельно.
* **`kind = triage`** — свой **симптом** для мастера «Что случилось?»: набор проверок только на чтение; результаты — ранжированные находки с действиями.

## Формат
```
# комментарий (строка целиком; также ; в начале строки)
[playbook]
id = repair-example            ; буквы, цифры, - и _
title = Название в списке
kind = repair                  ; repair | triage
risk = changing                ; safe | changing | danger (общий уровень)
needs = offline                ; target | offline | online | winpe | any
description = Первая строка
  продолжение строки (отступ в начале) добавляется к значению

[step]
type = message
text = Любой текст. {VOL} и другие токены подставляются.
```
Значения пишутся как есть (кавычки и обратные слэши не обрабатываются). Продолжение значения — строка с отступом.

## Токены `{ИМЯ}`
`{TARGET}` корень целевого тома (`C:\`), `{VOL}` (`C:`), `{WIN}` папка Windows, `{SYS32}` System32 среды, `{DATA}` папка Data, `{REPORT}` папка отчёта шага,
`{SCRATCH}` рабочая папка DISM, `{STAMP}` дата-время, `{LAST}` код возврата предыдущей команды, а также переменные из шагов `ask` и `set`.

## Типы шагов (`type`)
| type | Обязательные ключи | Описание |
|---|---|---|
| `message` | `text` | Вывод текста в журнал |
| `confirm` | `text` | Вопрос «Да/Нет»; «Нет» останавливает сценарий (`risk = danger` — строгое окно) |
| `ask` | `var`, `prompt` (`default`) | Запрос значения у пользователя → переменная `{VAR}` |
| `set` | `var`, `value` | Задать переменную |
| `cmd` | `run` | Запуск программы (`timeout`, `input`, `ok_codes = 0,3010`); путь ищется в System32/PATH |
| `catalog` | `no` | Команда каталога №N (`vol`, `win`, `src`, `host`, `efi`, `bcd` — параметры) |
| `fix` | `ids` | Исправления мастера (`1,2,3`) |
| `module` | `name` | `system, memory, boot, disk, drivers, events, integrity, bsod, dump, network` |
| `probe` | `name` | Встроенная проверка: `target, bitlocker, disks, nodriver, devices, boot, hives, pending, cbs, space, winre, dumps, events, persistence, policies, svcnet, svcupd, network` |
| `window` | `name` | Открыть окно: `storage, bcd, autoruns, registry, winre, fixes, triage, playbooks, catalog` |
| `check` | `check` | Своя проверка (ниже) — для `kind = triage` |
| `cmdcheck` | `run` (`contains`, `finding_if = found\|missing`) | Выполнить команду на чтение и создать находку по её выводу |

Общие необязательные ключи шага: `title`, `risk` (`safe | changing | danger`; для cmd/catalog/fix по умолчанию `changing`), `confirm = no` (не спрашивать для `changing`),
`when = ok | failed` (условие по предыдущему шагу), `onfail = stop | continue`.
**Любой шаг с `risk ≠ safe` перед выполнением подтверждается отдельным окном.** Весь сценарий перед запуском показывает план.

## Свои проверки (`type = check`)
Общие ключи находки: `severity = info|warn|bad`, `title`, `detail`, `action`, `action_label`, `okmsg` (показать, если проверка чистая).
* `check = file_exists` · `path` · `if = exists|missing`
* `check = reg_value` · `hive = software|system|ntuser` · `key` · `value` · `if = exists|missing|equals|notequals|contains|dword_ne0|dword_eq0` · `data`
  (для `system` начало `CurrentControlSet` подменяется на реальный ControlSet офлайн-ОС)
* `check = bugcheck` · `code = 0x7B` — последний дамп имеет этот BugCheck
* `check = event` · `log = System|Application` · `provider` · `id` · `min` · `days` · `levels = 1 or Level=2`
* `check = service_disabled` · `names = Dnscache, Dhcp`
* `check = space_below` · `gb = 15`

## Действия находки (`action = вид:значение`)
`catalog:18` · `fix:1,2` · `window:bcd` · `module:disk` · `playbook:repair-vmd-0x7b` — запуск сценария по идентификатору.

## Поставляемые примеры (папка `Playbooks\`)
`repair-vmd-0x7b` · `repair-pending-loop` · `repair-boot-uefi` · `repair-black-screen-login` · `repair-network-online` · `symptom-slow-boot` (свой симптом).

## Безопасность
Сценарий может запускать любые программы — используйте только сценарии из **доверенных** источников. Программа не выполняет рискованные шаги без вашего подтверждения,
а в режиме `/full` без окна такие шаги отклоняются автоматически. Все запуски пишутся в `Data\Logs\changes_*.log` (PLAYBOOK START/END, шаги).
