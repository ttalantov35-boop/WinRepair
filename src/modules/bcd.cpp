#include "bcd.h"

#include <winioctl.h>

namespace wr {

namespace {
std::vector<char> g_tempMounted;  // letters we mounted

char FreeLetter() {
    DWORD mask = GetLogicalDrives();
    for (int i = 25; i >= 3; --i) if (!(mask & (1u << i))) return (char)('A' + i);
    return 0;
}

void ScanLetter(char L, std::vector<BcdStore>& out) {
    std::string root = std::string(1, L) + ":\\";
    struct P { const char* rel; const char* kind; };
    for (P p : {P{"EFI\\Microsoft\\Boot\\BCD", "UEFI/ESP"}, P{"Boot\\BCD", "BIOS/MBR"}}) {
        if (!PathExists(root + p.rel)) continue;
        wchar_t fs[32] = {0}, lab[64] = {0};
        GetVolumeInformationW(W(root).c_str(), lab, 64, nullptr, nullptr, nullptr, fs, 32);
        ULARGE_INTEGER fb{}, tb{}, tf{};
        GetDiskFreeSpaceExW(W(root).c_str(), &fb, &tb, &tf);
        BcdStore s;
        s.path = root + p.rel;
        s.label = fmt("%s  [%s, %s %s, %s]", s.path.c_str(), p.kind, U(fs).c_str(), fmtBytes(tb.QuadPart).c_str(), U(lab).c_str());
        out.push_back(s);
    }
}
}  // namespace

std::string BcdExe() { return SysTool("bcdedit.exe"); }

std::vector<BcdStore> FindBcdStores() {
    std::vector<BcdStore> out;
    const Env& e = GetEnv();
    if (!e.isWinPE) { BcdStore s; s.path = ""; s.label = "(текущая система — хранилище по умолчанию)"; out.push_back(s); }
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        char L = (char)('A' + i);
        UINT dt = GetDriveTypeW(W(std::string(1, L) + ":\\").c_str());
        if (dt == DRIVE_FIXED || dt == DRIVE_REMOVABLE) ScanLetter(L, out);
    }
    // volumes without a drive letter (ESP / System Reserved)
    wchar_t vol[MAX_PATH];
    HANDLE h = FindFirstVolumeW(vol, MAX_PATH);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            DWORD need = 0;
            GetVolumePathNamesForVolumeNameW(vol, nullptr, 0, &need);
            std::vector<wchar_t> names(need + 2, 0);
            GetVolumePathNamesForVolumeNameW(vol, names.data(), need, &need);
            if (names[0]) continue;  // already has a mount point
            wchar_t fs[32] = {0};
            if (!GetVolumeInformationW(vol, nullptr, 0, nullptr, nullptr, nullptr, fs, 32)) continue;
            std::string f = toLowerUtf8(U(fs));
            if (f != "fat32" && f != "fat" && f != "ntfs") continue;
            ULARGE_INTEGER fb{}, tb{}, tf{};
            GetDiskFreeSpaceExW(vol, &fb, &tb, &tf);
            if (tb.QuadPart > 3ULL * 1024 * 1024 * 1024) continue;  // ESP / reserved volumes are small
            char L = FreeLetter();
            if (!L) break;
            std::wstring mp = std::wstring(1, (wchar_t)L) + L":\\";
            if (!SetVolumeMountPointW(mp.c_str(), vol)) continue;
            size_t before = out.size();
            ScanLetter(L, out);
            if (out.size() > before) {
                g_tempMounted.push_back(L);
                for (size_t i = before; i < out.size(); ++i) out[i].label += "  (временно смонтирован)";
                Logger::I().log(Lvl::Info, fmt("Временно смонтирован служебный том как %c: (будет отмонтирован при выходе)", L));
            } else DeleteVolumeMountPointW(mp.c_str());
        } while (FindNextVolumeW(h, vol, MAX_PATH));
        FindVolumeClose(h);
    }
    return out;
}

char TempMountVolume(const std::wstring& vol) {
    char L = FreeLetter();
    if (!L) return 0;
    std::wstring mp = std::wstring(1, (wchar_t)L) + L":\\";
    if (!SetVolumeMountPointW(mp.c_str(), vol.c_str())) return 0;
    g_tempMounted.push_back(L);
    return L;
}

void TempUnmountVolume(char L) {
    DeleteVolumeMountPointW((std::wstring(1, (wchar_t)L) + L":\\").c_str());
    for (size_t i = 0; i < g_tempMounted.size(); ++i) if (g_tempMounted[i] == L) { g_tempMounted.erase(g_tempMounted.begin() + (long)i); break; }
}

void UnmountTempBcdVolumes() {
    for (char L : g_tempMounted) DeleteVolumeMountPointW((std::wstring(1, (wchar_t)L) + L":\\").c_str());
    g_tempMounted.clear();
}

std::string BcdPrefix(const std::string& store) {
    std::string s = "\"" + BcdExe() + "\"";
    if (!store.empty()) s += " /store \"" + store + "\"";
    return s;
}

bool BcdRunSync(const std::string& store, const std::string& args, std::string& out, DWORD* code) {
    ProcResult r = RunProcess(BcdPrefix(store) + " " + args, "", nullptr, nullptr, 60);
    out = r.output;
    if (code) *code = r.exitCode;
    return r.started && r.exitCode == 0;
}

std::vector<BcdEntry> BcdEnum(const std::string& store, std::string& raw) {
    BcdRunSync(store, "/enum all /v", raw);
    return parseBcdEnum(raw);
}

std::string BcdBackup(const std::string& store, std::string& err) {
    if (store.empty()) {  // live store: let bcdedit export it
        std::string dst = BackupDirFor("", "bcd") + "bcd_live.bcd";
        if (!BackupOncePerSession("bcdlive:" + dst)) return dst;   // already exported in this session (pre-change state)
        std::string o;
        if (!BcdRunSync("", "/export \"" + dst + "\"", o)) { err = "bcdedit /export: " + o; return ""; }
        return dst;
    }
    std::string b = FileBackup(store, "BCD");
    if (b.empty()) err = "Не удалось скопировать " + store + ": " + ErrMsg(GetLastError());
    return b;
}

bool BcdRestore(const std::string& store, const std::string& backup, std::string& err) {
    if (store.empty()) {
        std::string o;
        if (!BcdRunSync("", "/import \"" + backup + "\"", o)) { err = o; return false; }
        return true;
    }
    if (!CopyFileW(W(backup).c_str(), W(store).c_str(), FALSE)) { err = ErrMsg(GetLastError()); return false; }
    return true;
}

const std::vector<BootMode>& BootModes() {
    static const std::vector<BootMode> v = {
        {"Безопасный режим (минимальный)", {{"safeboot", "minimal"}}, {"safebootalternateshell"}},
        {"Безопасный режим с загрузкой сетевых драйверов", {{"safeboot", "network"}}, {"safebootalternateshell"}},
        {"Безопасный режим с поддержкой командной строки", {{"safeboot", "minimal"}, {"safebootalternateshell", "yes"}}, {}},
        {"Режим восстановления службы каталогов (DSREPAIR)", {{"safeboot", "dsrepair"}}, {}},
        {"Вести журнал загрузки (ntbtlog.txt)", {{"bootlog", "yes"}}, {}},
        {"Включить видеорежим с низким разрешением (VGA)", {{"vga", "yes"}}, {}},
        {"Режим отладки", {{"debug", "yes"}}, {}},
        {"Отключить ранний запуск антивирусной защиты (ELAM)", {{"disableelamdrivers", "yes"}}, {}},
        {"Отключить обязательную проверку подписи драйверов", {{"nointegritychecks", "on"}}, {}},
        {"Тестовый режим подписи драйверов (testsigning)", {{"testsigning", "on"}}, {}},
        {"Показать расширенные параметры при следующей загрузке", {{"onetimeadvancedoptions", "on"}}, {}},
        {"Классическое меню загрузки F8 (legacy)", {{"bootmenupolicy", "legacy"}}, {}},
        {"Не запускать автовосстановление при сбоях загрузки", {{"bootstatuspolicy", "ignoreallfailures"}, {"recoveryenabled", "no"}}, {}},
        {"Без графической заставки загрузки (quietboot)", {{"quietboot", "yes"}}, {}},
        {"Показывать информацию о загрузке ОС (sos)", {{"sos", "yes"}}, {}},
    };
    return v;
}

std::vector<Step> BcdModeSteps(const std::string& store, const std::string& id, int idx) {
    std::vector<Step> v;
    const auto& m = BootModes();
    if (idx < 0 || idx >= (int)m.size()) return v;
    std::string pre = BcdPrefix(store);
    for (const char* d : m[(size_t)idx].deletes) v.push_back({fmt("Удалить параметр %s", d), pre + " /deletevalue " + id + " " + d, "", 30});
    for (auto& s : m[(size_t)idx].sets) v.push_back({fmt("Установить %s = %s", s.first, s.second), pre + " /set " + id + " " + s.first + " " + s.second, "", 30});
    return v;
}

std::vector<Step> BcdResetModesSteps(const std::string& store, const std::string& id) {
    std::vector<Step> v;
    std::string pre = BcdPrefix(store);
    for (const char* d : {"safeboot", "safebootalternateshell", "bootlog", "vga", "debug", "disableelamdrivers", "nointegritychecks", "testsigning",
                          "onetimeadvancedoptions", "bootstatuspolicy", "recoveryenabled", "quietboot", "sos"})
        v.push_back({fmt("Сбросить %s", d), pre + " /deletevalue " + id + " " + d, "", 30});
    v.push_back({"bootmenupolicy = standard", pre + " /set " + id + " bootmenupolicy standard", "", 30});
    return v;
}

void RunSteps(Ctx& c, const std::vector<Step>& steps) {
    int ok = 0, bad = 0;
    for (const auto& s : steps) {
        if (c.cancelled()) { c.warn("Прервано пользователем."); return; }
        AuditChange("STEP " + s.title + " :: " + s.cmd);
        ProcResult r = c.run(s.title, s.cmd, s.timeoutSec, s.input);
        // `bcdedit /deletevalue` of an absent element returns an error that is harmless when resetting modes
        if (r.started && r.exitCode == 0) ++ok; else ++bad;
    }
    c.info(fmt("Шагов выполнено успешно: %d, с ошибкой/код≠0: %d", ok, bad));
    if (bad == 0) c.ok("Готово.");
    else c.warn("Часть шагов вернула ошибку (для /deletevalue отсутствующего параметра это нормально).");
}

}  // namespace wr
