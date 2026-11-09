# Журнал изменений
Формат — [Keep a Changelog](https://keepachangelog.com/ru/1.1.0/), версии — [SemVer 2.0](https://semver.org/lang/ru/).

## [1.0.0-rc1]
### Добавлено
- Самопроверка целостности при запуске: SHA-256 всех файлов пакета по `MANIFEST.sha256`, состояние подписи Authenticode (`WinVerifyTrust`, загружается динамически).
  Изменённые исполняемые файлы — предупреждение и запись в журнал; изменённые сценарии/словари (их правят пользователи) — только отметка в журнале.
- `res/version.h` — единственный источник версии (код, ресурсы exe, пакет).
- `sign_exe.bat` (подпись SHA-256 + метка времени RFC 3161), `build_msvc_analyze.bat` (/analyze), поддержка ARM64 в `make_package.bat`, `MANIFEST.sha256` в пакете.
- Документы выпуска: RELEASE.md, RELEASE_CHECKLIST.md, PRIVACY.md, SECURITY.md, THIRD_PARTY_NOTICES.md, LICENSE.txt (шаблон).
### Исправлено
- Окно «Сценарии ремонта» было пустым при запуске `build\WinRepair.exe` из дерева исходников (папка `Playbooks` лежит уровнем выше). Теперь сценарии **встроены в exe** (RCDATA 100+), а `Playbooks\` рядом с exe, `..\Playbooks\` и `Data\Playbooks\` переопределяют встроенные по `id`.
### Ранее в 1.0
- Каталог команд, офлайн-реестр, BCD/загрузка, Autoruns/msconfig офлайн, WinRE, BitLocker и драйверы хранилища, мастер «Что случилось?», сценарии `.wrp`, локализация RU/EN.
