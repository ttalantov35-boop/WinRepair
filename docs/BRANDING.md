# Название, имя автора и логотип
* **Название и copyright** — в одном файле `res/version.h`: `WR_PRODUCT`, `WR_COMPANY`, `WR_COPYRIGHT`. Они попадают в заголовок окна, шапку, «Свойства» exe. Имена файлов пакета (`WinRepair_x64.exe`) задаются в `make_package.bat`.
* **Логотип:** `res/winrepair.ico` (в exe и окнах), PNG для сайта/GitHub — `docs/branding/`. Пересоздать: `python3 tools/make_icon.py` (нужен Pillow).
* **Цвета кнопок:** `src/ui_theme.cpp`, функция `Accent()`; назначение кнопки — `CatOfAction()` в `src/main.cpp`.
