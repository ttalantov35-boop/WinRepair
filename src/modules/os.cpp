// Integrity (DISM/SFC), repair, restore point, drivers.
#include "../win/app.h"

#include <cfgmgr32.h>
#include <setupapi.h>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")

namespace wr {

namespace {

std::string DismBase(const Ctx& c) {
    std::string s = "\"" + SysTool("dism.exe") + "\"";
    if (c.target.online) s += " /Online";
    else s += " /Image:" + c.target.root;
    return s;
}

std::string Scratch(const Ctx& c) {
    if (c.target.online) return "";
    std::string d = c.sub("scratch");
    d.pop_back();  // no trailing backslash inside quotes
    return " /ScratchDir:\"" + d + "\"";
}

std::string SfcArgs(const Ctx& c, const char* mode) {
    std::string s = "\"" + SysTool("sfc.exe") + "\" " + mode;
    if (!c.target.online) s += " /offbootdir=" + c.target.root + " /offwindir=" + c.target.winDir;
    return s;
}

}  // namespace

void ModIntegrity(Ctx& c) {
    c.info("=== Проверка целостности (без изменений) ===");
    if (!c.preflight(true, 0.5)) { c.error("Preflight не пройден."); return; }
    Json rep = Json::object();
    Section sec;
    sec.title = "Целостность";
    std::string all;
    bool bad = false;
    struct Step { std::string title, cmd; bool dismScan; };
    std::vector<Step> steps;
    steps.push_back({"DISM /CheckHealth", DismBase(c) + " /Cleanup-Image /CheckHealth" + Scratch(c), true});
    steps.push_back({"sfc /verifyonly", SfcArgs(c, "/verifyonly"), false});
    if (c.opt.extended) steps.push_back({"DISM /ScanHealth", DismBase(c) + " /Cleanup-Image /ScanHealth" + Scratch(c), true});
    steps.push_back({"DISM /AnalyzeComponentStore", DismBase(c) + " /Cleanup-Image /AnalyzeComponentStore" + Scratch(c), false});
    Json jst = Json::array();
    for (const auto& s : steps) {
        if (c.cancelled()) break;
        ProcResult r = c.run(s.title, s.cmd);
        all += "\r\n=== " + s.title + " ===\r\n" + r.output;
        bool flagged = (s.title != "DISM /AnalyzeComponentStore") && integrityViolationFound(r.output);
        if (flagged) { bad = true; c.warn("Обнаружены нарушения: " + s.title); }
        jst.add(Json::object().set("Title", s.title).set("ExitCode", (unsigned long)r.exitCode).set("Flagged", flagged));
        sec.rows.push_back({s.title, fmt("exit=%lu%s", (unsigned long)r.exitCode, flagged ? " — НАРУШЕНИЯ" : "")});
    }
    c.saveText("integrity.txt", all);
    rep.set("Steps", jst).set("Full", c.opt.extended).set("Violations", bad)
        .set("Recommendation", "Для исправления: кнопка «Исправить целостность (DISM + SFC)» — изменяющее действие.");
    c.saveText("integrity.json", rep.dump());
    if (bad) c.warn("Итог: нарушения целостности обнаружены.");
    else if (!c.cancelled()) c.ok("Итог: нарушений целостности не обнаружено.");
    if (c.res && !c.cancelled()) { c.res->haveIntegrity = true; c.res->integrityBad = bad; c.res->setSection(sec); }
}

void ModRepair(Ctx& c) {
    c.info("=== ИСПРАВЛЕНИЕ целостности: DISM /RestoreHealth -> SFC /scannow ===");
    if (!c.preflight(true, 2.0)) { c.error("Preflight не пройден — ремонт не запущен."); return; }
    std::string dism = DismBase(c) + " /Cleanup-Image /RestoreHealth" + Scratch(c);
    if (!c.opt.source.empty()) dism += " /Source:\"" + c.opt.source + "\"";
    ProcResult r = c.run("DISM /RestoreHealth", dism);
    c.saveText("repair-dism.txt", r.output);
    if (c.cancelled()) return;
    if (r.exitCode != 0) {
        c.error(fmt("DISM завершился с кодом 0x%08lX. SFC не запускается. 0x800F081F → несовместимый /Source.", (unsigned long)r.exitCode));
        c.error("Проверьте /Source: редакция/сборка должны совпадать с установленной.");
        return;
    }
    ProcResult s = c.run("sfc /scannow", SfcArgs(c, "/scannow"));
    c.saveText("repair-sfc.txt", s.output);
    if (c.cancelled()) return;
    if (s.exitCode == 0) c.ok("Ремонт завершён (sfc=0). Рекомендуется повторить проверку целостности и просмотреть журналы Setup/System.");
    else c.warn(fmt("SFC завершился с кодом %lu. Повторите проверку целостности.", (unsigned long)s.exitCode));
}

// ---- restore point ------------------------------------------------------------------------------
#pragma pack(push, 1)
struct RestorePtInfoW { DWORD dwEventType; DWORD dwRestorePtType; INT64 llSequenceNumber; WCHAR szDescription[256]; };
struct StateMgrStatus { DWORD nStatus; INT64 llSequenceNumber; };
#pragma pack(pop)

void ModRestorePoint(Ctx& c) {
    c.info("=== Точка восстановления ===");
    const Env& e = GetEnv();
    if (e.isWinPE || !c.target.online) { c.error("Точка восстановления доступна только для работающей (онлайн) Windows, не в WinPE/офлайн."); return; }
    if (!c.preflight(true, 2.0)) { c.error("Preflight не пройден."); return; }
    std::string desc = "WinRepair " + IsoNow();
    typedef BOOL(WINAPI * PSetRP)(RestorePtInfoW*, StateMgrStatus*);
    HMODULE m = LoadLibraryW(L"srclient.dll");
    PSetRP f = m ? (PSetRP)GetProcAddress(m, "SRSetRestorePointW") : nullptr;
    if (!f) { c.error("srclient.dll / SRSetRestorePointW недоступны (защита системы не установлена)."); return; }

    const char* key = "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\SystemRestore";
    DWORD oldFreq = 0;
    bool hadOld = RegDword(HKEY_LOCAL_MACHINE, key, "SystemRestorePointCreationFrequency", oldFreq);
    bool changed = false;
    if (c.opt.force && (!hadOld || oldFreq != 0)) {
        HKEY k;
        DWORD zero = 0;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, W(key).c_str(), 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
            RegSetValueExW(k, L"SystemRestorePointCreationFrequency", 0, REG_DWORD, (const BYTE*)&zero, sizeof zero);
            RegCloseKey(k);
            changed = true;
            c.info("Временно снято ограничение «1 точка в 24 часа».");
        }
    }
    RestorePtInfoW info{};
    StateMgrStatus st{};
    info.dwEventType = 100;      // BEGIN_SYSTEM_CHANGE
    info.dwRestorePtType = 12;   // MODIFY_SETTINGS
    wcsncpy(info.szDescription, W(desc).c_str(), 255);
    bool ok = f(&info, &st) != 0 && st.nStatus == 0;
    if (ok) {
        RestorePtInfoW endI{};
        StateMgrStatus st2{};
        endI.dwEventType = 101;  // END_SYSTEM_CHANGE
        endI.dwRestorePtType = 12;
        endI.llSequenceNumber = st.llSequenceNumber;
        f(&endI, &st2);
        c.ok("Точка восстановления создана: " + desc + ". Откат — rstrui.exe.");
    } else {
        c.error(fmt("Не удалось создать точку восстановления (статус %lu). Возможные причины: защита выключена, ограничение 24ч, мало места.", (unsigned long)st.nStatus));
    }
    if (changed) {  // restore previous value
        HKEY k;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, W(key).c_str(), 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
            if (hadOld) RegSetValueExW(k, L"SystemRestorePointCreationFrequency", 0, REG_DWORD, (const BYTE*)&oldFreq, sizeof oldFreq);
            else RegDeleteValueW(k, L"SystemRestorePointCreationFrequency");
            RegCloseKey(k);
        }
    }
}

// ---- drivers ----------------------------------------------------------------------------------------
void ModDrivers(Ctx& c) {
    c.info("=== Драйверы ===");
    std::string txt = "=== Driver Report " + IsoNow() + " ===\r\n";
    Json rep = Json::object();
    Section sec;
    sec.title = "Драйверы";

    // devices with problems (SetupAPI/CfgMgr32: same data as Get-PnpDevice -Status ERROR)
    Json jdev = Json::array();
    int errCount = 0;
    HDEVINFO h = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (h != INVALID_HANDLE_VALUE) {
        SP_DEVINFO_DATA dd{};
        dd.cbSize = sizeof dd;
        txt += "\r\n-- Устройства с ошибками --\r\n";
        for (DWORD i = 0; SetupDiEnumDeviceInfo(h, i, &dd); ++i) {
            ULONG status = 0, problem = 0;
            if (CM_Get_DevNode_Status(&status, &problem, dd.DevInst, 0) != CR_SUCCESS) continue;
            if (!(status & DN_HAS_PROBLEM) || problem == 0) continue;
            auto prop = [&](DWORD id) {
                wchar_t b[512] = {0};
                DWORD sz = 0;
                SetupDiGetDeviceRegistryPropertyW(h, &dd, id, nullptr, (PBYTE)b, sizeof b - 2, &sz);
                return U(b);
            };
            std::string name = prop(SPDRP_FRIENDLYNAME);
            if (name.empty()) name = prop(SPDRP_DEVICEDESC);
            std::string cls = prop(SPDRP_CLASS);
            wchar_t id[MAX_DEVICE_ID_LEN] = {0};
            CM_Get_Device_IDW(dd.DevInst, id, MAX_DEVICE_ID_LEN, 0);
            bool disabled = problem == 22;  // CM_PROB_DISABLED
            std::string line = fmt("[%s] %s | код проблемы %lu%s | %s", cls.c_str(), name.c_str(), (unsigned long)problem, disabled ? " (отключено)" : "", U(id).c_str());
            c.warn("Устройство: " + line);
            txt += line + "\r\n";
            jdev.add(Json::object().set("Class", cls).set("Name", name).set("Problem", (unsigned long)problem).set("Disabled", disabled).set("InstanceId", U(id)));
            sec.rows.push_back({name, fmt("код %lu", (unsigned long)problem)});
            if (!disabled) ++errCount;
        }
        SetupDiDestroyDeviceInfoList(h);
    } else c.warn("SetupAPI: не удалось перечислить устройства.");
    if (errCount == 0) c.ok("Устройств с ошибками драйверов не найдено.");
    rep.set("ErrorDevices", jdev);
    if (c.cancelled()) return;

    // installed driver packages
    {
        ProcResult r;
        if (c.target.online) r = c.run("pnputil /enum-drivers", "\"" + SysTool("pnputil.exe") + "\" /enum-drivers");
        else if (c.target.valid) r = c.run("DISM /get-drivers", "\"" + SysTool("dism.exe") + "\" /Image:" + c.target.root + " /Get-Drivers /all");
        txt += "\r\n-- Пакеты драйверов --\r\n" + r.output;
    }
    // Driver Verifier settings
    if (PathExists(SysDir() + "\\verifier.exe")) {
        ProcResult v = c.run("verifier /querysettings", "\"" + SysTool("verifier.exe") + "\" /querysettings");
        txt += "\r\n-- Driver Verifier --\r\n" + v.output;
    }
    c.saveText("drivers.txt", txt);
    c.saveText("drivers.json", rep.dump());
    if (c.res) { c.res->haveDrivers = true; c.res->errDevices = errCount; c.res->setSection(sec); }
}

}  // namespace wr
