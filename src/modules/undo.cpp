#include "undo.h"
#include "bcd.h"
#include "../win/offreg.h"
#include "../win/lang.h"

namespace wr {

namespace {

const char* KindTitle(const std::string& k) {
    if (k == "hive") return "Кусты реестра (копия целиком)";
    if (k == "reg") return "Ветки реестра";
    if (k == "bcd-live") return "Загрузка (BCD)";
    if (k == "file") return "Файлы";
    if (k == "quarantine") return "Карантин";
    return "Прочее";
}

// Текущий файл сохраняется рядом с суффиксом .before_undo: откат самого отката возможен вручную.
bool SafetyCopy(const std::string& path) {
    if (GetFileAttributesW(W(path).c_str()) == INVALID_FILE_ATTRIBUTES) return true;   // нечего сохранять
    return CopyFileW(W(path).c_str(), W(path + ".before_undo").c_str(), FALSE) != 0;
}

bool RestoreOne(const SessionChange& c, std::string& err) {
    if (c.kind == "quarantine") {
        if (PathExists(c.src)) { err = "путь уже существует: " + c.src; return false; }
        size_t k = c.src.find_last_of('\\');
        if (k != std::string::npos) MakeDirs(c.src.substr(0, k + 1));
        if (!MoveFileExW(W(c.backup).c_str(), W(c.src).c_str(), MOVEFILE_COPY_ALLOWED)) { err = ErrMsg(GetLastError()); return false; }
        return true;
    }
    if (c.kind == "file") {
        SafetyCopy(c.src);
        if (!CopyFileW(W(c.backup).c_str(), W(c.src).c_str(), FALSE)) { err = ErrMsg(GetLastError()); return false; }
        return true;
    }
    if (c.kind == "bcd-live") {
        return BcdRestore("", c.backup, err);
    }
    if (c.kind == "reg") {
        if (!c.online) { err = "офлайн-ветка: вернётся вместе с копией куста"; return false; }
        ProcResult r = RunProcess("\"" + SysTool("reg.exe") + "\" import \"" + c.backup + "\"", "", nullptr, nullptr, 120);
        if (!r.started || r.exitCode != 0) { err = "reg import: " + (r.output.empty() ? fmt("код %lu", (unsigned long)r.exitCode) : r.output); return false; }
        return true;
    }
    if (c.kind == "hive") {
        // выгрузить куст, если он сейчас загружен из этого файла
        std::string mount;
        for (const HiveInfo& h : Hives().loadedInfo())
            if (toLowerUtf8(h.file) == toLowerUtf8(c.src)) { mount = h.mount; break; }
        if (!mount.empty()) {
            Hives().unload(mount);
            if (Hives().isLoaded(mount)) { err = "куст не выгружается (занят): закройте окна реестра и повторите"; return false; }
        }
        SafetyCopy(c.src);
        if (!CopyFileW(W(c.backup).c_str(), W(c.src).c_str(), FALSE)) { err = ErrMsg(GetLastError()); return false; }
        return true;
    }
    err = "неизвестный тип записи: " + c.kind;
    return false;
}

}  // namespace

std::string UndoSummary(const std::vector<SessionChange>& v) {
    const char* order[] = {"quarantine", "file", "reg", "bcd-live", "hive"};
    std::string s;
    for (const char* k : order) {
        int n = 0;
        for (const auto& c : v) if (c.kind == k) ++n;
        if (!n) continue;
        s += Tr(KindTitle(k)) + fmt(" — %d:\n", n);
        int shown = 0;
        for (const auto& c : v) {
            if (c.kind != std::string(k)) continue;
            if (++shown > 5) { s += Tr(fmt("   … и ещё %d", n - 5)) + "\n"; break; }
            s += "   • " + Tr(c.what) + (c.src.empty() ? "" : "  (" + c.src + ")") + "\n";
        }
    }
    return s;
}

UndoReport UndoRestore(const std::vector<SessionChange>& v) {
    UndoReport r;
    // порядок: карантин и файлы, ветки реестра, BCD, в конце — кусты целиком
    const char* order[] = {"quarantine", "file", "reg", "bcd-live", "hive"};
    for (const char* k : order) {
        for (const auto& c : v) {
            if (c.kind != std::string(k)) continue;
            std::string err;
            if (RestoreOne(c, err)) {
                ++r.ok;
                std::string line = "Возвращено: " + c.what + (c.src.empty() ? "" : " -> " + c.src);
                r.text += "✔ " + Tr(line) + "\n";
                AuditChange("UNDO: " + line);
            } else if (c.kind == std::string("reg") && !c.online) {
                ++r.skipped;
                r.text += Tr("— Пропущено: " + c.what + " (" + err + ")") + "\n";
                Logger::I().log(Lvl::Info, "Откат: пропущено — " + c.what + ": " + err);
            } else {
                ++r.failed;
                r.text += Tr("✖ Ошибка: " + c.what + " — " + err) + "\n";
                Logger::I().log(Lvl::Error, "Откат: " + c.what + " — " + err);
            }
        }
    }
    return r;
}

}  // namespace wr
