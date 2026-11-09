// System snapshot, memory (SMBIOS), boot configuration. No WMI / PowerShell: works in WinPE.
#include "../win/app.h"
#include "../core/smbios.h"

#include <winioctl.h>

#include <cstring>

namespace wr {

bool GetSmbiosBlob(std::vector<uint8_t>& out) {
    UINT n = GetSystemFirmwareTable(0x52534D42 /*'RSMB'*/, 0, nullptr, 0);
    if (!n) return false;
    out.assign(n, 0);
    UINT m = GetSystemFirmwareTable(0x52534D42, 0, out.data(), n);
    if (!m) return false;
    out.resize(m);
    return true;
}

namespace {

std::string Uptime() {
    ULONGLONG ms = GetTickCount64();
    ULONGLONG s = ms / 1000;
    return fmt("%llu.%02llu:%02llu:%02llu", s / 86400, (s / 3600) % 24, (s / 60) % 60, s % 60);
}

int GetFwType() {  // 1=BIOS 2=UEFI 0=unknown
    typedef BOOL(WINAPI * PGetFirmwareType)(DWORD*);
    HMODULE k = GetModuleHandleW(L"kernel32.dll");
    PGetFirmwareType f = k ? (PGetFirmwareType)GetProcAddress(k, "GetFirmwareType") : nullptr;
    DWORD t = 0;
    if (f && f(&t)) return (int)t;
    return 0;
}

std::string SecureBootState() {
    DWORD v = 0;
    if (RegDword(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Control\\SecureBoot\\State", "UEFISecureBootEnabled", v))
        return v ? "Включён" : "Выключен";
    return "N/A (Legacy или не поддерживается)";
}

struct TpmInfo { bool present = false; unsigned version = 0; std::string text; };
TpmInfo GetTpm() {
    TpmInfo t;
    HMODULE m = LoadLibraryW(L"tbs.dll");
    if (!m) { t.text = "tbs.dll недоступна"; return t; }
    struct Info { UINT32 structVersion, tpmVersion, tpmInterfaceType, tpmImpRevision; };
    typedef UINT32(WINAPI * PGetInfo)(UINT32, PVOID);
    PGetInfo f = (PGetInfo)GetProcAddress(m, "Tbsi_GetDeviceInfo");
    if (f) {
        Info i{};
        UINT32 rc = f(sizeof i, &i);
        if (rc == 0) { t.present = true; t.version = i.tpmVersion; t.text = i.tpmVersion == 2 ? "TPM 2.0 обнаружен" : i.tpmVersion == 1 ? "TPM 1.2 обнаружен" : "TPM обнаружен"; }
        else t.text = rc == 0x8028400F ? "TPM не найден" : fmt("TBS: 0x%08X", rc);
    } else t.text = "Tbsi_GetDeviceInfo недоступна";
    FreeLibrary(m);
    return t;
}

}  // namespace

void ModSystem(Ctx& c) {
    c.info("=== Снимок системы ===");
    const Env& e = GetEnv();
    Json rep = Json::object();
    Section sec;
    sec.title = "Система";
    rep.set("Timestamp", IsoNow()).set("Computer", e.computer).set("IsAdmin", e.isAdmin).set("WinPE", e.isWinPE);

    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof ms;
    GlobalMemoryStatusEx(&ms);
    double ramGb = (double)ms.ullTotalPhys / (1024.0 * 1024 * 1024);
    Json os = Json::object();
    os.set("Caption", e.osName).set("Version", fmt("%u.%u", e.major, e.minor)).set("BuildNumber", e.build).set("Arch", e.arch)
        .set("Uptime", Uptime()).set("TotalRAM_GB", ramGb);
    DWORD inst = 0;
    if (RegDword(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "InstallDate", inst)) {
        unsigned long long ft = ((unsigned long long)inst + 11644473600ULL) * 10000000ULL;
        os.set("InstallDate", FileTimeStr(ft));
    }
    rep.set("OS", os);
    c.info(fmt("ОС: %s, %s%s", e.osName.c_str(), e.arch.c_str(), e.isWinPE ? " (WinPE)" : ""));
    c.info(fmt("Аптайм: %s, ОЗУ: %.1f ГБ, администратор: %s", Uptime().c_str(), ramGb, e.isAdmin ? "да" : "нет"));
    sec.rows.push_back({"ОС", e.osName + " " + e.arch});
    sec.rows.push_back({"Аптайм", Uptime()});
    sec.rows.push_back({"ОЗУ", fmt("%.1f ГБ", ramGb)});
    if (c.target.valid) sec.rows.push_back({"Целевая ОС", c.target.label});

    std::vector<uint8_t> blob;
    SmbInfo sm;
    if (GetSmbiosBlob(blob)) sm = parseSmbios(blob.data(), blob.size());
    if (sm.ok) {
        Json bios = Json::object();
        bios.set("Manufacturer", sm.biosVendor).set("Version", sm.biosVersion).set("ReleaseDate", sm.biosDate)
            .set("BaseBoard", trim(sm.boardMfr + " " + sm.boardProduct)).set("System", trim(sm.sysMfr + " " + sm.sysProduct));
        rep.set("BIOS", bios);
        c.info(fmt("BIOS: %s %s (%s)", sm.biosVendor.c_str(), sm.biosVersion.c_str(), sm.biosDate.c_str()));
        c.info(fmt("Плата: %s; система: %s", trim(sm.boardMfr + " " + sm.boardProduct).c_str(), trim(sm.sysMfr + " " + sm.sysProduct).c_str()));
        sec.rows.push_back({"BIOS", fmt("%s %s (%s)", sm.biosVendor.c_str(), sm.biosVersion.c_str(), sm.biosDate.c_str())});
        sec.rows.push_back({"Материнская плата", trim(sm.boardMfr + " " + sm.boardProduct)});
    } else {
        rep.set("BIOS", "ERROR: SMBIOS недоступен");
        c.warn("SMBIOS недоступен.");
    }
    int fw = GetFwType();
    rep.set("Firmware", fw == 2 ? "UEFI" : fw == 1 ? "BIOS (Legacy)" : "?");
    std::string sb = SecureBootState();
    rep.set("SecureBoot", sb);
    c.info(fmt("Прошивка: %s, Secure Boot: %s", fw == 2 ? "UEFI" : fw == 1 ? "Legacy BIOS" : "?", sb.c_str()));
    sec.rows.push_back({"Secure Boot", sb});
    TpmInfo tpm = GetTpm();
    rep.set("TPM", Json::object().set("Present", tpm.present).set("Version", tpm.version).set("Info", tpm.text));
    c.info("TPM: " + tpm.text);
    sec.rows.push_back({"TPM", tpm.text});

    Json mem = Json::array();
    for (const auto& m : sm.mem) {
        if (!m.present) continue;
        mem.add(Json::object().set("BankLabel", m.bank).set("DeviceLocator", m.locator).set("CapacityMB", (unsigned long long)m.sizeMB)
                    .set("Speed", m.speed).set("Manufacturer", m.manufacturer).set("PartNumber", m.part));
        c.info(fmt("ОЗУ %s: %llu МБ %s %u МГц %s %s", m.locator.c_str(), (unsigned long long)m.sizeMB, smbMemTypeName(m.type), m.speed,
                   m.manufacturer.c_str(), m.part.c_str()));
    }
    rep.set("Memory", mem);

    std::vector<DiskInfo> disks = CollectDisks();
    Json jd = Json::array(), jr = Json::array();
    for (const auto& d : disks) {
        jd.add(Json::object().set("Number", d.number).set("FriendlyName", d.model).set("HealthStatus", d.health).set("PartitionStyle", d.partStyle)
                   .set("SizeGB", (double)d.size / (1024.0 * 1024 * 1024)));
        jr.add(Json::object().set("Disk", d.model).set("Temperature", d.temp).set("Wear", d.wear).set("ReadErr", d.readErr).set("WriteErr", d.writeErr));
        c.info(fmt("Диск %d: %s, %s, %s, %s", d.number, d.model.c_str(), fmtBytes(d.size).c_str(), d.partStyle.c_str(), d.health.c_str()));
    }
    rep.set("Disks", jd).set("Reliability", jr);

    c.saveText("system.json", rep.dump());
    if (c.res) c.res->setSection(sec);
    c.ok("Снимок системы: готово.");
}

void ModMemory(Ctx& c) {
    c.info("=== Память ===");
    Json rep = Json::object();
    rep.set("Timestamp", IsoNow());
    Section sec;
    sec.title = "Память";
    std::vector<uint8_t> blob;
    SmbInfo sm;
    if (GetSmbiosBlob(blob)) sm = parseSmbios(blob.data(), blob.size());
    Json mods = Json::array();
    std::vector<unsigned> speeds;
    std::vector<std::string> parts;
    unsigned long long totalMB = 0;
    for (const auto& m : sm.mem) {
        if (!m.present) continue;
        totalMB += m.sizeMB;
        mods.add(Json::object().set("BankLabel", m.bank).set("DeviceLocator", m.locator).set("CapacityMB", (unsigned long long)m.sizeMB)
                     .set("Type", smbMemTypeName(m.type)).set("Speed", m.speed).set("ConfiguredClockSpeed", m.configSpeed)
                     .set("Manufacturer", m.manufacturer).set("PartNumber", m.part).set("SerialNumber", m.serial));
        c.info(fmt("%s (%s): %llu МБ %s, %u МГц (работает %u), %s %s", m.locator.c_str(), m.bank.c_str(), (unsigned long long)m.sizeMB,
                   smbMemTypeName(m.type), m.speed, m.configSpeed, m.manufacturer.c_str(), m.part.c_str()));
        sec.rows.push_back({m.locator, fmt("%llu МБ %s %u МГц %s", (unsigned long long)m.sizeMB, smbMemTypeName(m.type), m.speed, m.part.c_str())});
        if (m.speed && std::find(speeds.begin(), speeds.end(), m.speed) == speeds.end()) speeds.push_back(m.speed);
        if (!m.part.empty() && std::find(parts.begin(), parts.end(), m.part) == parts.end()) parts.push_back(m.part);
    }
    rep.set("Modules", mods);
    if (!sm.ok) c.warn("SMBIOS недоступен — данные о модулях ОЗУ получить не удалось.");
    rep.set("Array", Json::object().set("MemoryDevices", sm.memSlots).set("MaxCapacityKB", (unsigned long long)sm.maxCapKB));
    c.info(fmt("Слотов: %u, установлено: %llu МБ", sm.memSlots, totalMB));

    // Windows Memory Diagnostic results
    std::vector<EvtRow> rows;
    std::string err;
    Json jdiag = Json::array();
    if (c.target.valid &&
        QueryEvents(c, "System", "*[System[Provider[@Name='Microsoft-Windows-MemoryDiagnostics-Results']]]", 20, true, false, rows, err)) {
        for (const auto& r : rows) {
            c.info(fmt("MemDiag %s #%u %s: %s", r.time.c_str(), r.id, r.level.c_str(), truncUtf8(r.message, 160).c_str()));
            jdiag.add(Json::object().set("Time", r.time).set("Id", r.id).set("Level", r.level).set("Message", r.message));
        }
        if (rows.empty()) c.info("Результатов Windows Memory Diagnostic нет.");
    }
    rep.set("Diagnostics", jdiag);

    Json rec = Json::array();
    auto addRec = [&](const std::string& s) { rec.add(s); c.warn("Рекомендация: " + s); sec.rows.push_back({"Рекомендация", s}); };
    if (speeds.size() > 1) {
        std::string s;
        for (unsigned v : speeds) s += (s.empty() ? "" : ", ") + std::to_string(v);
        addRec("Разные частоты: " + s + " МГц.");
    }
    if (parts.size() > 1) addRec("Разные партномера: возможна нестабильная пара.");
    addRec("При подозрении: тест JEDEC (XMP/DOCP/EXPO выкл.), несколько проходов.");
    addRec("Один прогон без ошибок НЕ доказывает исправность.");
    rep.set("Recommendation", rec);
    c.saveText("memory.json", rep.dump());
    if (c.res) c.res->setSection(sec);
    c.ok("Память: готово.");
}

void ModBoot(Ctx& c) {
    c.info("=== Загрузка: BCD / GPT / ESP / WinRE ===");
    if (!c.needTarget()) return;
    const Env& e = GetEnv();
    Json rep = Json::object();
    rep.set("Timestamp", IsoNow());
    std::vector<std::string> issues;
    bool pass = true;
    Section sec;
    sec.title = "Загрузка";

    // BCD
    if (c.target.online) {
        ProcResult r = c.run("bcdedit /enum {current}", "\"" + SysTool("bcdedit.exe") + "\" /enum {current}");
        rep.set("Bcd", Json::object().set("ExitCode", (unsigned long)r.exitCode).set("Raw", r.output));
        std::string l = toLowerUtf8(r.output);
        if (!r.started || r.exitCode != 0 || containsAny(l, {"не удалось", "not found", "access is denied", "отказано"})) {
            issues.push_back("BCD: ошибка чтения {current}.");
            pass = false;
        }
        c.saveText("bcd.txt", r.output);
    } else {
        c.info("Офлайн-режим: хранилище BCD расположено на ESP/системном разделе, а не в Windows-томе. "
               "Проверьте вручную: bcdedit /store <ESP>\\EFI\\Microsoft\\Boot\\BCD /enum all");
        rep.set("Bcd", "пропущено (офлайн)");
    }
    if (c.cancelled()) return;

    // disk layout
    int dn = VolumeDiskNumber(c.target.letter());
    std::vector<uint8_t> lay;
    int fw = 0;
    {
        typedef BOOL(WINAPI * PGetFirmwareType)(DWORD*);
        HMODULE k = GetModuleHandleW(L"kernel32.dll");
        PGetFirmwareType f = k ? (PGetFirmwareType)GetProcAddress(k, "GetFirmwareType") : nullptr;
        DWORD t = 0;
        if (f && f(&t)) fw = (int)t;
    }
    if (dn >= 0 && GetDiskLayout(dn, lay)) {
        auto* L = (DRIVE_LAYOUT_INFORMATION_EX*)lay.data();
        bool gpt = L->PartitionStyle == PARTITION_STYLE_GPT;
        c.info(fmt("Системный диск: Disk %d, стиль разделов %s, firmware: %s", dn, gpt ? "GPT" : "MBR", fw == 2 ? "UEFI" : fw == 1 ? "BIOS" : "?"));
        rep.set("DiskMode", Json::object().set("PartitionStyle", gpt ? "GPT" : "MBR").set("DiskNumber", dn));
        sec.rows.push_back({"Стиль разделов", gpt ? "GPT" : "MBR"});
        if (gpt) {
            static const GUID kEsp = {0xC12A7328, 0xF81F, 0x11D2, {0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B}};
            bool found = false;
            for (DWORD i = 0; i < L->PartitionCount; ++i) {
                const PARTITION_INFORMATION_EX& p = L->PartitionEntry[i];
                if (p.PartitionStyle == PARTITION_STYLE_GPT && memcmp(&p.Gpt.PartitionType, &kEsp, sizeof(GUID)) == 0) {
                    found = true;
                    c.info(fmt("ESP: раздел %lu, %llu МБ", (unsigned long)p.PartitionNumber, (unsigned long long)p.PartitionLength.QuadPart / (1024 * 1024)));
                    rep.set("Esp", Json::object().set("PartitionNumber", (unsigned long)p.PartitionNumber).set("SizeMB", (unsigned long long)p.PartitionLength.QuadPart / (1024 * 1024)));
                    sec.rows.push_back({"ESP", fmt("раздел %lu, %llu МБ", (unsigned long)p.PartitionNumber, (unsigned long long)p.PartitionLength.QuadPart / (1024 * 1024))});
                }
            }
            if (!found) { issues.push_back("GPT-диск, но ESP не найден."); pass = false; }
        } else {
            issues.push_back("Диск в MBR: Windows 11 требует GPT+UEFI.");
            if (fw == 1) {
                bool active = false;
                for (DWORD i = 0; i < L->PartitionCount; ++i)
                    if (L->PartitionEntry[i].PartitionStyle == PARTITION_STYLE_MBR && L->PartitionEntry[i].Mbr.BootIndicator) active = true;
                if (!active) { issues.push_back("MBR+BIOS: нет активного раздела."); pass = false; }
            }
        }
    } else {
        issues.push_back("Не удалось прочитать таблицу разделов системного диска.");
    }
    if (c.cancelled()) return;

    // WinRE
    std::string cmd = "\"" + SysTool("reagentc.exe") + "\" /info";
    if (!c.target.online) cmd += " /target \"" + c.target.winDir + "\"";
    ProcResult re = c.run("reagentc /info", cmd);
    std::string rl = toLowerUtf8(re.output);
    bool enabled = containsAny(rl, {"enabled", "включено"});
    rep.set("WinRE", Json::object().set("Raw", re.output).set("Enabled", enabled));
    if (re.started && re.exitCode == 0 && !enabled) issues.push_back("WinRE отключён (reagentc /info).");
    else if (!re.started || re.exitCode != 0) c.warn("reagentc недоступен или вернул ошибку — состояние WinRE не определено.");
    sec.rows.push_back({"WinRE", enabled ? "включён" : "отключён/неизвестно"});
    (void)e;

    Json ji = Json::array();
    for (const auto& s : issues) { ji.add(s); c.warn("Замечание: " + s); sec.rows.push_back({"Замечание", s}); }
    rep.set("Issues", ji).set("Pass", pass);
    if (issues.empty()) c.ok("Загрузка: замечаний нет.");
    c.saveText("boot.json", rep.dump());
    if (c.res) { c.res->haveBoot = true; c.res->bootIssues = (int)issues.size(); c.res->setSection(sec); }
}

}  // namespace wr
