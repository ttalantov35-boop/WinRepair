# Как выложить WinRepair на GitHub — пошагово

Инструкция для человека, который раньше не работал с GitHub. Всё делается на вашем Windows-компьютере. Время: 30–40 минут.

## Что вы получите
- страницу проекта `https://github.com/<ваш-ник>/WinRepair` с кодом и историей изменений;
- автоматическую проверку: при каждой загрузке GitHub сам соберёт `WinRepair.exe` на Windows и запустит тесты;
- «Релизы»: нажали одну кнопку (поставили тег) — GitHub собрал x64+x86, упаковал zip и вложил хэш SHA-256.

## Шаг 0. Решите заранее
1. **Лицензия** (обязательно): откройте `docs/LICENSING_AND_DISTRIBUTION.md` и выберите. Готовые тексты: `docs/license-options/MIT.txt` (открытый код, проще всего) или `Freeware-EULA.txt` (бесплатно, но нельзя менять). Скопируйте выбранный файл поверх `LICENSE.txt` в корне проекта и замените `<ВАШЕ ИМЯ>` и год. Файл с надписью «ШАБЛОН» выкладывать нельзя.
2. **Публичный или приватный репозиторий.** Рекомендую сначала **Private**, проверить, что всё собралось, и только потом сменить на Public (Settings → General → Danger Zone → Change visibility).
3. **Имя в exe**: в `res/version.h` поля `WR_COMPANY` и `WR_COPYRIGHT` — впишите своё имя или оставьте «WinRepair Project».
4. **Данные `bsod-codes.json`**: убедитесь, что вправе их публиковать (см. `THIRD_PARTY_NOTICES.md`).

## Шаг 1. Аккаунт GitHub
1. Откройте https://github.com/signup, введите почту, пароль, имя пользователя (оно будет в адресе проекта), подтвердите почту.
2. Включите двухфакторную защиту: Settings → Password and authentication → Enable two-factor authentication (приложение-аутентификатор). Без неё GitHub может ограничить действия.
3. **Скрыть личную почту.** Settings → Emails → отметьте «Keep my email addresses private» и «Block command line pushes that expose my email». Запомните адрес вида `12345678+ник@users.noreply.github.com` (показан на этой странице) — он понадобится в шаге 3.

## Шаг 2. Установите Git
1. Скачайте «Git for Windows» с https://git-scm.com/download/win и установите, везде нажимая Next (настройки по умолчанию подходят; Git Credential Manager включён по умолчанию — он сам откроет браузер для входа).
2. Откройте меню Пуск → **Git Bash** (или PowerShell) и проверьте: `git --version`.

## Шаг 3. Представьтесь Git
В Git Bash выполните (подставьте своё):
```
git config --global user.name "Ваше Имя"
git config --global user.email "12345678+ник@users.noreply.github.com"
git config --global init.defaultBranch main
```
Почему noreply: адрес в коммитах виден всем в интернете. В уже сделанных коммитах стоит другой адрес (см. шаг 5, «Адрес в старых коммитах»).

## Шаг 4. Подготовьте папку проекта
1. Распакуйте `WinRepairGUI.zip`, например в `C:\Projects\WinRepair`. В архиве уже есть скрытая папка `.git` с историей (коммиты и тег `v1.0.0-rc1`).
2. Замените `LICENSE.txt` (шаг 0). Выполните в папке проекта:
```
cd /c/Projects/WinRepair
git status
git add LICENSE.txt res/version.h
git commit -m "Set license and product identity"
```
3. Если папки `.git` нет (распаковали без скрытых файлов) — выполните там `git init`, `git add .`, `git commit -m "Initial commit"`.

## Шаг 5. Создайте пустой репозиторий на GitHub
1. На github.com нажмите **+** → **New repository**.
2. Repository name: `WinRepair`. Description: «Offline Windows repair toolkit (WinPE/WinRE) — registry, BCD, autoruns, apps, explorer».
3. Выберите **Private** (или Public). **Не отмечайте** «Add a README», «.gitignore», «license» — они уже есть в проекте, иначе будет конфликт.
4. Нажмите **Create repository**. Откроется страница с адресом `https://github.com/<ник>/WinRepair.git`.

**Адрес в старых коммитах (по желанию).** Если не хотите, чтобы старый адрес почты был виден в истории, до загрузки выполните один раз (история небольшая):
```
git filter-branch -f --env-filter 'export GIT_AUTHOR_EMAIL="12345678+ник@users.noreply.github.com"; export GIT_COMMITTER_EMAIL="12345678+ник@users.noreply.github.com"' -- --all
```
Хэши коммитов изменятся — делайте это ДО первой загрузки. Тег `v1.0.0-rc1` после этого удалите и поставьте заново: `git tag -d v1.0.0-rc1` и `git tag v1.0.0-rc1`.

## Шаг 6. Загрузите проект
```
git remote add origin https://github.com/<ник>/WinRepair.git
git branch -M main
git push -u origin main
git push origin --tags
```
При первой загрузке откроется окно входа в браузере — войдите и разрешите доступ (Git Credential Manager). Если просит пароль в консоли: обычный пароль не подойдёт, нужен **Personal Access Token** (Settings → Developer settings → Personal access tokens → Fine-grained → доступ только к этому репозиторию, право Contents: Read and write) — вставьте его вместо пароля.

Обновите страницу репозитория — файлы должны появиться.

## Шаг 7. Проверьте автосборку
1. Вкладка **Actions** → workflow **CI**. Он стартует сам после загрузки: «Core tests» (Linux, тесты) и «Build WinRepair.exe (MSVC x64)» (Windows).
2. Зелёная галочка — всё собралось; в завершённом запуске внизу **Artifacts** лежит `WinRepair-x64-…` с exe.
3. Красный крест — откройте запуск → нажмите на упавший шаг → скопируйте текст ошибки и пришлите мне; исправлю. (Это первый настоящий прогон сборки не на вашем компьютере, поэтому ошибки возможны.)
4. Если Actions выключены: Settings → Actions → General → Allow all actions.

## Шаг 8. Оформите страницу проекта
- **About** (шестерёнка справа вверху): описание, теги `windows`, `winpe`, `winre`, `repair`, `registry`, `bcd`, `offline`, `autoruns`.
- **README.md** уже показывается на главной странице.
- Settings → General → Features: включите Issues (для сообщений об ошибках), по желанию Discussions.
- Settings → Code security: включите «Private vulnerability reporting» (соответствует `SECURITY.md`).
- Settings → Branches → Add rule для `main`: «Require a pull request» — если будут участники; для работы в одиночку можно пропустить.

## Шаг 9. Сделайте первый релиз
1. Поднимите версию в `res/version.h` (например `1.0.0`; `WR_VERSION_STR "1.0.0"`), запишите изменения в `CHANGELOG.md`, затем:
```
git add -A
git commit -m "Release 1.0.0"
git push
git tag v1.0.0
git push origin v1.0.0
```
2. Вкладка **Actions** → workflow **Release**: соберёт x64 и x86, сделает `WinRepair-v1.0.0.zip` и файл `.sha256`.
3. Вкладка **Releases** (справа на главной) — там появится релиз со скачиваемым zip и описанием изменений (оно создаётся автоматически). Проверьте и при необходимости отредактируйте текст (кнопка карандаша). Для версий-кандидатов (`-rc1`) отметьте «Set as a pre-release».
4. В тексте релиза напишите SHA-256 архива (файл `.sha256` рядом) — пользователи смогут сверить скачанное.

Примечание: подпись кода в CI не выполняется (нужен сертификат). Сертификат и его пароль **никогда не кладите в репозиторий**; если подпись понадобится, их хранят в Settings → Secrets and variables → Actions.

## Шаг 10. Повседневная работа
```
git pull                 # забрать изменения
# ...правите файлы...
./run_tests.sh           # проверка (в Git Bash / WSL)
git add -A
git commit -m "Что изменили"
git push
```
Лучше работать в ветках: `git switch -c fix/registry-d-drive` → push → на GitHub кнопка «Compare & pull request» → после зелёного CI «Merge».
Сообщения об ошибках приходят во вкладку Issues (шаблон просит версию, среду, шаги и журнал).

## Вариант без командной строки
Установите **GitHub Desktop** (https://desktop.github.com): File → Add local repository → выберите папку проекта → Publish repository (снимите галочку Public для приватного). Дальше: изменили файлы → внизу слева описание → Commit → Push origin.

## Частые проблемы
| Симптом | Решение |
|---|---|
| `remote: Repository not found` | Неверный адрес или нет доступа: проверьте `git remote -v`, вход под нужным аккаунтом |
| `rejected … fetch first` | В репозитории создали README при создании. `git pull --rebase origin main` и снова `git push` |
| `GH007: push would publish a private email` | Включили блокировку почты (шаг 1.3): повторите шаг 3 с noreply-адресом и перезапишите коммиты (шаг 5) |
| CI «Build» красный | Откройте лог шага «Build», пришлите ошибку |
| Антивирус ругается на exe | Нормально для неподписанных утилит ремонта: подпишите exe и сообщайте ложные срабатывания производителю антивируса (`SECURITY.md`) |
| Файл больше 100 МБ | GitHub не принимает; большие файлы выкладывайте только в Releases |

## Чек-лист перед публикацией в Public
- [ ] `LICENSE.txt` — настоящая лицензия, без слова «ШАБЛОН»
- [ ] нет паролей, ключей, `.pfx`, личных путей и логов (`git grep -i "password\|pfx"`)
- [ ] права на `bsod-codes.json` и иные данные подтверждены
- [ ] CI зелёный, релиз скачивается и запускается в WinPE
- [ ] README и CHANGELOG актуальны
