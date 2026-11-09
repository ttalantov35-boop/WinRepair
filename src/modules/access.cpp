#include "access.h"

#include <cctype>
#include <cfgmgr32.h>
#include <setupapi.h>

namespace wr {

namespace {

std::string LineVal(const std::string& out, std::initializer_list<const char*> keys) {
    size_t p = 0;
    while (p < out.size()) {
        size_t e = out.find('\n', p);
        if (e == std::string::npos) e = out.size();
        std::string line = out.substr(p, e - p);
        p = e + 1;
        std::string l = toLowerUtf8(line);
        for (const char* k : keys) {
            if (l.find(k) == std::string::npos) continue;
            size_t c = line.find(':');
            if (c != std::string::npos) return trim(line.substr(c + 1));
        }
    }
    return "";
}

std::string BdeExe() { return SysTool("manage-bde.exe"); }

}  // namespace

bool BlToolPresent() { return PathExists(SysDir() + "\\manage-bde.exe") || !FindInPath("manage-bde.exe").empty(); }

std::string MaskSecrets(const std::string& line) {
    std::string o = line;
    // 6 digits '-' repeated: mask every digit group that is followed or preceded by '-' + 6 digits
    for (size_t i = 0; i + 6 <= o.size(); ++i) {
        bool six = true;
        for (size_t k = 0; k < 6; ++k) if (!isdigit((unsigned char)o[i + k])) { six = false; break; }
        if (!six) continue;
        bool dashAfter = i + 7 < o.size() && o[i + 6] == '-' && isdigit((unsigned char)o[i + 7]);
        bool dashBefore = i >= 7 && o[i - 1] == '-' && isdigit((unsigned char)o[i - 2]);
        if (dashAfter || dashBefore) { for (size_t k = 0; k < 6; ++k) o[i + k] = '*'; i += 5; }
    }
    return o;
}

std::string NormalizeRecoveryPassword(const std::string& in) {
    std::string d;
    for (char c : in) if (isdigit((unsigned char)c)) d += c;
    if (d.size() != 48) return "";
    std::string o;
    for (int g = 0; g < 8; ++g) { if (g) o += '-'; o += d.substr((size_t)g * 6, 6); }
    return o;
}

std::vector<BlVolume> BlScan() {
    std::vector<BlVolume> v;
    if (!BlToolPresent()) return v;
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        char L = (char)('A' + i);
        std::string root = std::string(1, L) + ":\\";
        UINT dt = GetDriveTypeW(W(root).c_str());
        if (dt != DRIVE_FIXED && dt != DRIVE_REMOVABLE) continue;
        BlVolume b;
        b.letter = L;
        ProcResult r = RunProcess("\"" + BdeExe() + "\" -status " + std::string(1, L) + ":", "", nullptr, nullptr, 25);
        b.raw = r.output;
        b.lockStatus = LineVal(r.output, {"lock status", "состояние блокировки"});
        b.protection = LineVal(r.output, {"protection status", "состояние защиты"});
        b.conversion = LineVal(r.output, {"conversion status", "состояние преобразования"});
        b.known = r.started && (!b.lockStatus.empty() || !b.conversion.empty());
        std::string ll = toLowerUtf8(b.lockStatus), cl = toLowerUtf8(b.conversion);
        if (ll.find("unlocked") != std::string::npos || ll.find("разблокирован") != std::string::npos) b.locked = false;
        else b.locked = ll.find("locked") != std::string::npos || ll.find("заблокирован") != std::string::npos;
        b.encrypted = !cl.empty() && cl.find("decrypted") == std::string::npos && cl.find("расшифрован") == std::string::npos;
        // a volume the file system cannot open at all is locked even when manage-bde output is not understood
        wchar_t fs[32] = {0};
        if (!GetVolumeInformationW(W(root).c_str(), nullptr, 0, nullptr, nullptr, nullptr, fs, 32) && !b.known) { b.locked = true; b.encrypted = true; }
        v.push_back(b);
    }
    return v;
}

bool BlUnlock(Ctx& c, char letter, int kind, const std::string& secret) {
    std::string vol = std::string(1, letter) + ":", arg, shown;
    if (kind == 0) {
        std::string n = NormalizeRecoveryPassword(secret);
        if (n.empty()) { c.error("Пароль восстановления должен содержать 48 цифр (8 групп по 6)."); return false; }
        arg = "-RecoveryPassword " + n;
        shown = "-RecoveryPassword ******-******-******-******-******-******-******-******";
    } else if (kind == 1) {
        if (secret.empty() || secret.find('"') != std::string::npos) { c.error("Пароль пуст или содержит кавычки."); return false; }
        arg = "-Password"; shown = "-Password ********";
    } else {
        if (!PathExists(secret)) { c.error("Файл ключа не найден: " + secret); return false; }
        arg = "-RecoveryKey \"" + secret + "\""; shown = arg;
    }
    c.info("▶ manage-bde -unlock " + vol + " " + shown);
    AuditChange("BitLocker: попытка разблокировки тома " + vol + " (способ " + std::to_string(kind) + ", секрет не записывается)");
    std::string cmd = "\"" + BdeExe() + "\" -unlock " + vol + " " + arg;
    ProcResult r;
    if (kind == 1) {
        // -Password asks interactively: feed it through stdin
        cmd = "\"" + BdeExe() + "\" -unlock " + vol + " -Password";
        r = RunProcess(cmd, "", [](const std::string& l, bool p) { if (!p) Logger::I().log(Lvl::Out, "  | " + MaskSecrets(l)); }, c.cancel, 120, secret + "\r\n");
    } else {
        r = RunProcess(cmd, "", [](const std::string& l, bool p) { if (!p) Logger::I().log(Lvl::Out, "  | " + MaskSecrets(l)); }, c.cancel, 120);
    }
    if (r.started && r.exitCode == 0) {
        AuditChange("BitLocker: том " + vol + " разблокирован");
        c.ok("Том " + vol + " разблокирован. Нажмите «Обновить» в главном окне, чтобы найти Windows на этом томе.");
        return true;
    }
    if (kind == 1) c.warn("manage-bde может не принимать пароль через конвейер (читает его с консоли) — надёжнее пароль восстановления (48 цифр) или файл ключа .bek.");
    c.error(fmt("Не удалось разблокировать том %s (код %lu). Проверьте ключ и букву тома. manage-bde есть не во всех WinPE (нужен компонент BitLocker).", vol.c_str(), (unsigned long)r.exitCode));
    return false;
}

void BlRunSafe(Ctx& c, const std::string& title, const std::string& args) {
    c.info("▶ " + title);
    std::string cmd = "\"" + BdeExe() + "\" " + args;
    ProcResult r = RunProcess(cmd, "", [](const std::string& l, bool p) { if (!p) Logger::I().log(Lvl::Out, "  | " + MaskSecrets(l)); }, c.cancel, 120);
    if (!r.started) c.error("manage-bde недоступен: " + r.error);
    else c.info(fmt("  код возврата: %lu", (unsigned long)r.exitCode));
}

// ---- storage drivers ---------------------------------------------------------------------------------------------------
static void ScanInf(const std::string& dir, int depth, const std::atomic<bool>* cancel, int maxResults, std::vector<std::string>& out) {
    if (depth > 6 || (int)out.size() >= maxResults || (cancel && cancel->load())) return;
    for (const auto& f : ListFiles(dir, "*.inf")) {
        std::vector<uint8_t> raw;
        if (!ReadBytes(dir + f, raw, 1 << 18) || raw.empty()) continue;
        std::string t = (raw.size() > 2 && raw[0] == 0xFF && raw[1] == 0xFE) ? utf16leToUtf8(raw.data(), raw.size()) : std::string((const char*)raw.data(), raw.size());
        std::string l = toLowerUtf8(t);
        size_t cp = l.find("class");
        bool storage = false;
        while (cp != std::string::npos && !storage) {
            size_t e = l.find('\n', cp);
            std::string line = l.substr(cp, (e == std::string::npos ? l.size() : e) - cp);
            if (line.find('=') != std::string::npos && (line.find("scsiadapter") != std::string::npos || line.find("hdc") != std::string::npos || line.find("storage") != std::string::npos)) storage = true;
            cp = l.find("class", cp + 5);
        }
        if (storage) { out.push_back(dir + f); if ((int)out.size() >= maxResults) return; }
    }
    for (const auto& d : ListDirs(dir)) ScanInf(dir + d + "\\", depth + 1, cancel, maxResults, out);
}

std::vector<std::string> FindStorageInfs(const std::string& folder, const std::atomic<bool>* cancel, int maxResults) {
    std::vector<std::string> out;
    std::string f = folder;
    if (!f.empty() && f.back() != '\\') f += '\\';
    ScanInf(f, 0, cancel, maxResults, out);
    return out;
}

int NoDriverDevices(std::vector<std::string>& lines, bool& vmd) {
    vmd = false;
    HDEVINFO h = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (h == INVALID_HANDLE_VALUE) return 0;
    SP_DEVINFO_DATA dd{};
    dd.cbSize = sizeof dd;
    int n = 0;
    static const char* vmdIds[] = {"DEV_9A0B", "DEV_467F", "DEV_A77F", "DEV_7D0B"};
    for (DWORD i = 0; SetupDiEnumDeviceInfo(h, i, &dd); ++i) {
        ULONG st = 0, pr = 0;
        if (CM_Get_DevNode_Status(&st, &pr, dd.DevInst, 0) != CR_SUCCESS || !(st & DN_HAS_PROBLEM) || pr != 28) continue;
        wchar_t nm[512] = {0}, hw[1024] = {0};
        DWORD sz = 0;
        SetupDiGetDeviceRegistryPropertyW(h, &dd, SPDRP_DEVICEDESC, nullptr, (PBYTE)nm, sizeof nm - 2, &sz);
        SetupDiGetDeviceRegistryPropertyW(h, &dd, SPDRP_HARDWAREID, nullptr, (PBYTE)hw, sizeof hw - 4, &sz);
        std::string id = U(hw), up = id;
        for (auto& ch : up) ch = (char)toupper((unsigned char)ch);
        if (up.find("VEN_8086") != std::string::npos) for (const char* v : vmdIds) if (up.find(v) != std::string::npos) vmd = true;
        lines.push_back(U(nm) + " | " + id);
        ++n;
    }
    SetupDiDestroyDeviceInfoList(h);
    return n;
}

void NatNoDriverDevices(Ctx& c, OutBuf& o) {
    HDEVINFO h = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (h == INVALID_HANDLE_VALUE) { c.error("SetupDiGetClassDevs: " + ErrMsg(GetLastError())); return; }
    SP_DEVINFO_DATA dd{};
    dd.cbSize = sizeof dd;
    int missing = 0, storage = 0;
    bool vmd = false;
    static const char* vmdIds[] = {"DEV_9A0B", "DEV_467F", "DEV_A77F", "DEV_7D0B"};
    o.add("Устройства без установленного драйвера (код проблемы 28) и контроллеры хранилища:");
    for (DWORD i = 0; SetupDiEnumDeviceInfo(h, i, &dd); ++i) {
        auto prop = [&](DWORD id) {
            wchar_t b[1024] = {0};
            DWORD sz = 0;
            SetupDiGetDeviceRegistryPropertyW(h, &dd, id, nullptr, (PBYTE)b, sizeof b - 2, &sz);
            return U(b);
        };
        ULONG st = 0, pr = 0;
        bool prob = CM_Get_DevNode_Status(&st, &pr, dd.DevInst, 0) == CR_SUCCESS && (st & DN_HAS_PROBLEM);
        std::string cls = prop(SPDRP_CLASS);
        std::string lc = toLowerUtf8(cls);
        bool isStorage = lc == "scsiadapter" || lc == "hdc" || lc == "diskdrive";
        if (!(prob && pr == 28) && !(isStorage && lc != "diskdrive")) continue;
        std::string name = prop(SPDRP_FRIENDLYNAME);
        if (name.empty()) name = prop(SPDRP_DEVICEDESC);
        wchar_t hw[2048] = {0};
        DWORD sz = 0;
        SetupDiGetDeviceRegistryPropertyW(h, &dd, SPDRP_HARDWAREID, nullptr, (PBYTE)hw, sizeof hw - 4, &sz);
        std::string hid = U(hw);  // first hardware id
        std::string up = hid;
        for (auto& ch : up) ch = (char)toupper((unsigned char)ch);
        std::string tag = prob && pr == 28 ? "[НЕТ ДРАЙВЕРА] " : "[контроллер]    ";
        o.add(tag + name + (cls.empty() ? "" : "  (класс " + cls + ")") + "\r\n      ID: " + hid);
        if (prob && pr == 28) ++missing; else ++storage;
        if (up.find("VEN_8086") != std::string::npos) for (const char* v : vmdIds) if (up.find(v) != std::string::npos) vmd = true;
    }
    SetupDiDestroyDeviceInfoList(h);
    o.add(fmt("Без драйвера: %d; контроллеров хранилища с драйвером: %d", missing, storage));
    if (vmd) o.add("ПОДСКАЗКА: обнаружен контроллер Intel VMD/RST (по ID устройства). Для видимости NVMe-дисков загрузите драйвер Intel RST/VMD: «Загрузить драйвер в WinPE (drvload)».");
    else if (missing) o.add("ПОДСКАЗКА: найдите драйвер по Hardware ID (VEN_xxxx&DEV_xxxx) и загрузите его через drvload; затем нажмите «Обновить» в главном окне.");
    else o.add("Устройств с отсутствующим драйвером не найдено.");
}

}  // namespace wr
