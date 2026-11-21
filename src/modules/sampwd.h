// Снятие пароля и разблокировка локальных учётных записей через SAM офлайн-Windows.
// Снятие пароля = обнуление длины NT/LM-хеша в значении V (работает Win7–Win11, независимо от формата хеша).
// Включение / разблокировка = сброс битов ACB в значении F. Новый пароль здесь НЕ задаётся (отдельный пакет).
#pragma once
#include "../win/app.h"

namespace wr {

// Одна локальная учётная запись из SAM.
struct SamAccount {
    unsigned rid = 0;
    std::string name;         // имя входа (UTF-8)
    std::string fullName;     // полное имя, может быть пустым
    bool disabled = false;    // ACB_DISABLED
    bool locked = false;      // ACB_AUTOLOCK
    bool pwdNotReq = false;   // ACB_PWNOTREQ
    bool hasPassword = false; // длина NT-хеша > 0
    bool microsoft = false;   // похоже на учётную запись Microsoft (офлайн-сброс ненадёжен)
    bool builtinAdmin = false; // RID 500 — встроенный Администратор
    bool guest = false;        // RID 501 — Гость
};

// Прочитать локальные учётные записи из SAM выбранной (офлайн) Windows. Только чтение.
// Возвращает false и текст ошибки в err при неудаче.
bool SamListAccounts(const Target& t, std::vector<SamAccount>& out, std::string& err);

// Операции над одной учётной записью.
enum class SamOp {
    Blank,              // снять пароль
    Enable,             // включить учётную запись
    Unlock,             // снять блокировку
    BlankEnableUnlock,  // снять пароль + включить + разблокировать (для забытого пароля)
};

// Выполнить операцию op над учётной записью rid в SAM цели c.target.
// Запускается заданием (UiStartJob): пишет ход в журнал, новый пароль не задаётся и не логируется.
bool SamApply(Ctx& c, unsigned rid, SamOp op, std::string& err);

}  // namespace wr
