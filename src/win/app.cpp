#include "app.h"

namespace wr {

ProcResult Ctx::run(const std::string& title, const std::string& cmd, int timeoutSec, const std::string& input) const {
    info("▶ " + title);
    Logger::I().log(Lvl::Info, "  команда: " + cmd);
    ULONGLONG t0 = GetTickCount64();
    ProcResult r = RunProcess(
        cmd, "",
        [](const std::string& line, bool progress) {
            if (progress) Logger::I().progress(line);
            else Logger::I().log(Lvl::Out, "  | " + line);
        },
        cancel, timeoutSec, input);
    double sec = (double)(GetTickCount64() - t0) / 1000.0;
    if (!r.started) error("Не удалось запустить: " + r.error);
    else if (r.cancelled) warn(fmt("Отменено пользователем (%.1f с)", sec));
    else if (r.timedOut) warn(fmt("Таймаут %d с — процесс завершён", timeoutSec));
    else info(fmt("  код возврата: %lu (0x%08lX), %.1f с", (unsigned long)r.exitCode, (unsigned long)r.exitCode, sec));
    Logger::I().progress("");
    return r;
}

std::string Ctx::sub(const std::string& name) const {
    std::string d = outDir + name + "\\";
    MakeDirs(d);
    return d;
}

bool Ctx::needTarget() const {
    if (!target.valid) {
        error("Не выбрана целевая ОС. Выберите установленную Windows в списке «Целевая ОС» (кнопка «Обновить»).");
        return false;
    }
    return true;
}

void Ctx::saveText(const std::string& file, const std::string& text) const {
    std::string p = outDir + file;
    if (WriteText(p, text)) info("Сохранено: " + p);
    else error("Не удалось записать " + p);
}

bool Ctx::preflight(bool needAdmin, double minFreeGB) const {
    const Env& e = GetEnv();
    bool okAll = true;
    if (needAdmin && !e.isAdmin) { error("Preflight: требуются права администратора."); okAll = false; }
    if (!needTarget()) return false;
    ULARGE_INTEGER freeB{}, tot{}, tf{};
    if (GetDiskFreeSpaceExW(W(target.root).c_str(), &freeB, &tot, &tf)) {
        double gb = (double)freeB.QuadPart / (1024.0 * 1024 * 1024);
        if (gb < minFreeGB) { error(fmt("Preflight: на %s свободно %.1f ГБ, нужно не менее %.1f ГБ.", target.root.c_str(), gb, minFreeGB)); okAll = false; }
    }
    if (target.online && !e.isWinPE) {
        if (RegKeyExists(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Component Based Servicing\\RebootPending") ||
            RegKeyExists(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\WindowsUpdate\\Auto Update\\RebootRequired") ||
            RegValueExists(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Control\\Session Manager", "PendingFileRenameOperations"))
            warn("Preflight: в системе ожидается перезагрузка — результаты DISM/SFC могут быть неточными.");
        SYSTEM_POWER_STATUS ps{};
        if (GetSystemPowerStatus(&ps) && ps.ACLineStatus == 0 && ps.BatteryLifePercent != 255 && ps.BatteryLifePercent < 30)
            warn("Preflight: работа от батареи (<30%). Подключите питание перед ремонтом.");
    }
    return okAll;
}

}  // namespace wr
