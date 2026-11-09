// Disks: native IOCTL enumeration, SMART (SATA + NVMe), volumes, chkdsk, FS events, SMART delta.
#include "../win/app.h"

#include <winioctl.h>
#include <ntddstor.h>

#include <cstddef>

namespace wr {

namespace {

HANDLE OpenDisk(int n, bool* rw = nullptr) {
    std::wstring p = L"\\\\.\\PhysicalDrive" + std::to_wstring(n);
    DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE;
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ | GENERIC_WRITE, share, nullptr, OPEN_EXISTING, 0, nullptr);
    bool ok = h != INVALID_HANDLE_VALUE;
    if (!ok) h = CreateFileW(p.c_str(), GENERIC_READ, share, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) h = CreateFileW(p.c_str(), 0, share, nullptr, OPEN_EXISTING, 0, nullptr);
    if (rw) *rw = ok;
    return h;
}

bool QueryProp(HANDLE h, STORAGE_PROPERTY_ID id, std::vector<uint8_t>& out, DWORD* ret = nullptr) {
    STORAGE_PROPERTY_QUERY q{};
    q.PropertyId = id;
    q.QueryType = PropertyStandardQuery;
    out.assign(4096, 0);
    DWORD r = 0;
    BOOL ok = DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, out.data(), (DWORD)out.size(), &r, nullptr);
    if (ret) *ret = r;
    return ok != 0;
}

std::string DescStr(const STORAGE_DEVICE_DESCRIPTOR* d, DWORD off, DWORD total) {
    if (!off || off >= total) return "";
    const char* s = (const char*)d + off;
    size_t maxn = total - off;
    size_t n = 0;
    while (n < maxn && s[n]) ++n;
    return trim(std::string(s, n));
}

const char* BusName(int b) {
    static const char* n[] = {"Unknown", "SCSI", "ATAPI", "ATA", "1394", "SSA", "Fibre", "USB", "RAID",
                              "iSCSI", "SAS", "SATA", "SD", "MMC", "Virtual", "FileBackedVirtual", "Spaces", "NVMe"};
    return (b >= 0 && b < (int)(sizeof n / sizeof n[0])) ? n[b] : "Other";
}

uint64_t LE64(const uint8_t* p) { uint64_t v = 0; for (int i = 7; i >= 0; --i) v = (v << 8) | p[i]; return v; }

bool NvmeHealth(HANDLE h, uint8_t* log512) {
    const DWORD len = (DWORD)(sizeof(STORAGE_PROPERTY_QUERY) + sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA) + 512);
    std::vector<uint8_t> buf(len, 0);
    auto* q = (STORAGE_PROPERTY_QUERY*)buf.data();
    q->PropertyId = StorageDeviceProtocolSpecificProperty;
    q->QueryType = PropertyStandardQuery;
    auto* p = (STORAGE_PROTOCOL_SPECIFIC_DATA*)q->AdditionalParameters;
    p->ProtocolType = (STORAGE_PROTOCOL_TYPE)3;  // ProtocolTypeNvme
    p->DataType = 2;                              // NVMeDataTypeLogPage
    p->ProtocolDataRequestValue = 2;              // NVME_LOG_PAGE_HEALTH_INFO
    p->ProtocolDataRequestSubValue = 0;
    p->ProtocolDataOffset = sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA);
    p->ProtocolDataLength = 512;
    DWORD ret = 0;
    if (!DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, buf.data(), len, buf.data(), len, &ret, nullptr)) return false;
    auto* d = (STORAGE_PROTOCOL_DATA_DESCRIPTOR*)buf.data();
    auto* sp = &d->ProtocolSpecificData;
    if (sp->ProtocolDataOffset < sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA) || sp->ProtocolDataLength < 512) return false;
    if ((size_t)((const uint8_t*)sp - buf.data()) + sp->ProtocolDataOffset + 512 > buf.size()) return false;
    memcpy(log512, (const uint8_t*)sp + sp->ProtocolDataOffset, 512);
    return true;
}

bool SataSmart(HANDLE h, uint8_t* attrs512) {
    SENDCMDINPARAMS in{};
    std::vector<uint8_t> out(sizeof(SENDCMDOUTPARAMS) + 512, 0);
    in.cBufferSize = 512;
    in.irDriveRegs.bFeaturesReg = 0xD0;      // SMART READ DATA
    in.irDriveRegs.bSectorCountReg = 1;
    in.irDriveRegs.bSectorNumberReg = 1;
    in.irDriveRegs.bCylLowReg = 0x4F;
    in.irDriveRegs.bCylHighReg = 0xC2;
    in.irDriveRegs.bDriveHeadReg = 0xA0;
    in.irDriveRegs.bCommandReg = 0xB0;       // SMART
    in.bDriveNumber = 0;
    DWORD ret = 0;
    if (!DeviceIoControl(h, SMART_RCV_DRIVE_DATA, &in, sizeof(SENDCMDINPARAMS) - 1, out.data(), (DWORD)out.size(), &ret, nullptr)) return false;
    auto* o = (SENDCMDOUTPARAMS*)out.data();
    memcpy(attrs512, o->bBuffer, 512);
    return true;
}

}  // namespace

bool GetDiskLayout(int disk, std::vector<uint8_t>& buf) {
    HANDLE h = OpenDisk(disk);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = false;
    for (DWORD sz = 1 << 16; sz <= (1u << 20); sz <<= 2) {
        buf.assign(sz, 0);
        DWORD ret = 0;
        if (DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, nullptr, 0, buf.data(), sz, &ret, nullptr)) { ok = true; break; }
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) break;
    }
    CloseHandle(h);
    return ok;
}

int VolumeDiskNumber(char letter) {
    std::wstring p = L"\\\\.\\";
    p += (wchar_t)letter;
    p += L":";
    HANDLE h = CreateFileW(p.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return -1;
    std::vector<uint8_t> b(1024, 0);
    DWORD ret = 0;
    int res = -1;
    if (DeviceIoControl(h, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0, b.data(), (DWORD)b.size(), &ret, nullptr)) {
        auto* e = (VOLUME_DISK_EXTENTS*)b.data();
        if (e->NumberOfDiskExtents >= 1) res = (int)e->Extents[0].DiskNumber;
    }
    CloseHandle(h);
    return res;
}

std::vector<DiskInfo> CollectDisks() {
    std::vector<DiskInfo> v;
    for (int n = 0; n < 64; ++n) {
        bool rw = false;
        HANDLE h = OpenDisk(n, &rw);
        if (h == INVALID_HANDLE_VALUE) continue;
        DiskInfo d;
        d.number = n;
        std::vector<uint8_t> b;
        DWORD ret = 0;
        int bus = 0;
        if (QueryProp(h, StorageDeviceProperty, b, &ret)) {
            auto* sd = (STORAGE_DEVICE_DESCRIPTOR*)b.data();
            std::string vend = DescStr(sd, sd->VendorIdOffset, ret), prod = DescStr(sd, sd->ProductIdOffset, ret);
            d.model = trim(vend + (vend.empty() || prod.empty() ? "" : " ") + prod);
            d.serial = DescStr(sd, sd->SerialNumberOffset, ret);
            bus = (int)sd->BusType;
            d.bus = BusName(bus);
        }
        if (d.model.empty()) d.model = fmt("Disk %d", n);
        GET_LENGTH_INFORMATION li{};
        if (DeviceIoControl(h, IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0, &li, sizeof li, &ret, nullptr)) d.size = (unsigned long long)li.Length.QuadPart;
        // media type
        bool seekKnown = false, seek = false;
        if (QueryProp(h, StorageDeviceSeekPenaltyProperty, b, &ret) && ret >= sizeof(DEVICE_SEEK_PENALTY_DESCRIPTOR)) {
            seekKnown = true;
            seek = ((DEVICE_SEEK_PENALTY_DESCRIPTOR*)b.data())->IncursSeekPenalty != 0;
        }
        d.media = bus == 17 ? "SSD (NVMe)" : seekKnown ? (seek ? "HDD" : "SSD") : "?";
        // partition style
        std::vector<uint8_t> lay;
        if (GetDiskLayout(n, lay)) {
            auto* L = (DRIVE_LAYOUT_INFORMATION_EX*)lay.data();
            d.partStyle = L->PartitionStyle == PARTITION_STYLE_GPT ? "GPT" : L->PartitionStyle == PARTITION_STYLE_MBR ? "MBR" : "RAW";
        } else d.partStyle = "?";
        // predict failure
        STORAGE_PREDICT_FAILURE pf{};
        if (DeviceIoControl(h, IOCTL_STORAGE_PREDICT_FAILURE, nullptr, 0, &pf, sizeof pf, &ret, nullptr) && pf.PredictFailure) {
            d.health = "Warning";
            d.note += "SMART: прогноз отказа. ";
        }
        // SMART details
        if (bus == 17) {
            uint8_t log[512];
            if (NvmeHealth(h, log)) {
                unsigned kelvin = log[1] | (log[2] << 8);
                if (kelvin > 273) d.temp = (int)kelvin - 273;
                d.wear = log[5];
                d.powerOnHours = (long long)LE64(log + 128);
                d.readErr = (long long)LE64(log + 160);
                d.writeErr = 0;
                if (log[0] & 0x1F) { d.health = "Warning"; d.note += fmt("NVMe critical warning=0x%02X. ", log[0] & 0x1F); }
                if (log[3] < log[4]) { d.health = "Warning"; d.note += "Резерв ниже порога. "; }
            } else d.note += "Счётчики NVMe недоступны. ";
        } else if (rw && (bus == 3 || bus == 11 || bus == 1 || bus == 8)) {
            uint8_t a[512];
            if (SataSmart(h, a)) {
                long long pending = 0, unc = 0;
                int wearVal = -1;
                for (int i = 0; i < 30; ++i) {
                    const uint8_t* e = a + 2 + i * 12;
                    int id = e[0];
                    if (!id) continue;
                    int value = e[3];
                    uint32_t raw32 = e[5] | (e[6] << 8) | ((uint32_t)e[7] << 16) | ((uint32_t)e[8] << 24);
                    if ((id == 194 || id == 190) && d.temp < 0) d.temp = e[5];
                    else if (id == 9) d.powerOnHours = raw32;
                    else if (id == 197) pending = raw32;
                    else if (id == 198) unc = raw32;
                    else if (id == 5 && raw32 > 0) d.note += fmt("Переназначено секторов: %u. ", raw32);
                    else if ((id == 177 || id == 231 || id == 233) && wearVal < 0 && value >= 1 && value <= 100) wearVal = 100 - value;
                }
                if (d.media != "HDD" && wearVal >= 0) d.wear = wearVal;
                d.readErr = pending + unc;  // pending + offline-uncorrectable sectors
                d.writeErr = -1;
            } else d.note += "SMART-атрибуты недоступны. ";
        } else if (bus != 17) {
            d.note += "Счётчики надёжности недоступны для этой шины. ";
        }
        CloseHandle(h);
        v.push_back(d);
    }
    return v;
}

// ---------------------------------------------------------------------------------------------

void ModDisk(Ctx& c) {
    c.info("=== Диски: SMART, тома, ошибки ФС ===");
    Json rep = Json::object();
    rep.set("Timestamp", IsoNow());
    Section sec;
    sec.title = "Диски";

    std::vector<DiskInfo> disks = CollectDisks();
    Json jd = Json::array();
    for (const auto& d : disks) {
        c.info(fmt("Disk %d: %s | %s | %s | %s | %s | health=%s", d.number, d.model.c_str(), d.bus.c_str(), d.media.c_str(),
                   fmtBytes(d.size).c_str(), d.partStyle.c_str(), d.health.c_str()));
        std::string det;
        if (d.temp >= 0) det += fmt("T=%d°C ", d.temp);
        if (d.wear >= 0) det += fmt("износ=%d%% ", d.wear);
        if (d.powerOnHours >= 0) det += fmt("наработка=%lld ч ", d.powerOnHours);
        if (d.readErr >= 0) det += fmt("ошибки(R/pending)=%lld ", d.readErr);
        if (d.writeErr >= 0) det += fmt("ошибки(W)=%lld ", d.writeErr);
        if (!det.empty() || !d.note.empty()) c.info("   " + det + d.note);
        Json o = Json::object();
        o.set("Number", d.number).set("Model", d.model).set("Serial", d.serial).set("Bus", d.bus).set("MediaType", d.media)
            .set("PartitionStyle", d.partStyle).set("SizeBytes", d.size).set("HealthStatus", d.health)
            .set("TemperatureC", d.temp).set("WearPercent", d.wear).set("PowerOnHours", d.powerOnHours)
            .set("ReadErrors", d.readErr).set("WriteErrors", d.writeErr).set("Note", d.note);
        jd.add(o);
        sec.rows.push_back({fmt("Disk %d: %s", d.number, d.model.c_str()),
                            fmt("%s, %s, %s, Health=%s%s", d.bus.c_str(), d.media.c_str(), fmtBytes(d.size).c_str(), d.health.c_str(),
                                d.wear >= 0 ? fmt(", износ %d%%", d.wear).c_str() : "")});
    }
    if (disks.empty()) c.warn("Физические диски не обнаружены (нет драйвера хранилища?).");
    rep.set("Disks", jd);
    if (c.cancelled()) return;

    // volumes
    Json jv = Json::array();
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        char L = (char)('A' + i);
        std::wstring root = std::wstring(1, (wchar_t)L) + L":\\";
        UINT dt = GetDriveTypeW(root.c_str());
        if (dt != DRIVE_FIXED && dt != DRIVE_REMOVABLE) continue;
        wchar_t label[128] = {0}, fsn[64] = {0};
        DWORD ser = 0, maxc = 0, fl = 0;
        GetVolumeInformationW(root.c_str(), label, 128, &ser, &maxc, &fl, fsn, 64);
        ULARGE_INTEGER fb{}, tb{}, tf{};
        GetDiskFreeSpaceExW(root.c_str(), &fb, &tb, &tf);
        // dirty bit
        std::wstring dev = std::wstring(L"\\\\.\\") + (wchar_t)L + L":";
        HANDLE hv = CreateFileW(dev.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        std::string dirty = "?";
        if (hv != INVALID_HANDLE_VALUE) {
            DWORD flags = 0, ret = 0;
            if (DeviceIoControl(hv, FSCTL_IS_VOLUME_DIRTY, nullptr, 0, &flags, sizeof flags, &ret, nullptr)) dirty = (flags & 1) ? "ДА" : "нет";
            CloseHandle(hv);
        }
        c.info(fmt("Том %c: [%s] %s, %s из %s свободно, dirty=%s", L, U(label).c_str(), U(fsn).c_str(),
                   fmtBytes(fb.QuadPart).c_str(), fmtBytes(tb.QuadPart).c_str(), dirty.c_str()));
        if (dirty == "ДА") c.warn(fmt("Том %c: установлен dirty-бит — рекомендуется chkdsk.", L));
        Json o = Json::object();
        o.set("Drive", std::string(1, L)).set("Label", U(label)).set("FileSystem", U(fsn)).set("SizeBytes", (unsigned long long)tb.QuadPart)
            .set("FreeBytes", (unsigned long long)fb.QuadPart).set("Dirty", dirty);
        jv.add(o);
    }
    rep.set("Volumes", jv);

    // chkdsk /scan (read-only online scan)
    if (c.opt.extended && c.needTarget()) {
        std::string dr = std::string(1, c.target.letter()) + ":";
        ProcResult r = c.run("chkdsk " + dr + " /scan", "\"" + SysTool("chkdsk.exe") + "\" " + dr + " /scan");
        c.saveText("chkdsk.txt", r.output);
        rep.set("ChkScan", Json::object().set("Drive", dr).set("ExitCode", (unsigned long)r.exitCode));
        sec.rows.push_back({"chkdsk /scan " + dr, fmt("exit=%lu", (unsigned long)r.exitCode)});
    } else if (!c.opt.extended) {
        c.info("chkdsk /scan пропущен (включите «Расширенный режим»).");
    }
    if (c.cancelled()) return;

    // FS-related events
    std::vector<EvtRow> rows;
    std::string err;
    std::string cond = "(Provider[@Name='disk'] or Provider[@Name='Ntfs'] or Provider[@Name='Microsoft-Windows-Ntfs'] or Provider[@Name='stornvme'] or Provider[@Name='storahci'])";
    if (c.target.valid && QueryEvents(c, "System", XpathRecent(c.opt.days, cond), 500, true, false, rows, err)) {
        c.info(fmt("События дисков/ФС за %d дн.: %zu", c.opt.days, rows.size()));
        std::string csv = "Time,Id,Level,Provider,Message\r\n";
        Json je = Json::array();
        for (size_t i = 0; i < rows.size(); ++i) {
            const auto& e = rows[i];
            csv += csvQuote(e.time) + "," + std::to_string(e.id) + "," + csvQuote(e.level) + "," + csvQuote(e.provider) + "," + csvQuote(e.message) + "\r\n";
            if (i < 8) c.info(fmt("  %s  %s #%u  %s", e.time.c_str(), e.provider.c_str(), e.id, truncUtf8(e.message, 120).c_str()));
            if (i < 200) je.add(Json::object().set("Time", e.time).set("Id", e.id).set("Level", e.level).set("Provider", e.provider).set("Message", e.message));
        }
        c.saveText("fs-events.csv", csv);
        rep.set("FsEvents", je);
        sec.rows.push_back({fmt("События дисков/ФС (%d дн.)", c.opt.days), std::to_string(rows.size())});
        if (rows.size() > 50) c.warn("Много событий дисков/ФС — проверьте кабель/накопитель/питание.");
    } else if (!err.empty()) c.warn("Журнал событий: " + err);

    // SMART delta vs previous run
    std::string hist = DataDir() + "SmartHistory\\";
    MakeDirs(hist);
    std::vector<std::string> files = ListFiles(hist, "smart_*.csv");
    std::sort(files.begin(), files.end());
    Json jdelta = Json::array();
    if (!files.empty()) {
        std::vector<uint8_t> raw;
        if (ReadBytes(hist + files.back(), raw, 1 << 20)) {
            std::string txt((const char*)raw.data(), raw.size());
            size_t p = 0;
            while (p < txt.size()) {
                size_t e = txt.find('\n', p);
                if (e == std::string::npos) e = txt.size();
                std::string line = trim(txt.substr(p, e - p));
                p = e + 1;
                std::vector<std::string> f;
                size_t s = 0;
                while (true) { size_t q = line.find('|', s); if (q == std::string::npos) { f.push_back(line.substr(s)); break; } f.push_back(line.substr(s, q - s)); s = q + 1; }
                if (f.size() < 5) continue;
                for (const auto& d : disks) {
                    std::string key = d.serial.empty() ? d.model : d.serial;
                    if (key != f[0]) continue;
                    long long dr = d.readErr - atoll(f[2].c_str()), dw = d.writeErr - atoll(f[3].c_str());
                    int dwear = d.wear - atoi(f[4].c_str());
                    if (d.readErr >= 0 && atoll(f[2].c_str()) >= 0 && (dr != 0 || dw != 0 || dwear != 0)) {
                        c.warn(fmt("SMART-дельта %s: ошибки %+lld, износ %+d%% с прошлого запуска", d.model.c_str(), dr, dwear));
                        jdelta.add(Json::object().set("Disk", d.model).set("ReadErrDelta", dr).set("WriteErrDelta", dw).set("WearDelta", dwear));
                    }
                }
            }
        }
    }
    rep.set("SmartDelta", jdelta);
    std::string snap;
    for (const auto& d : disks)
        snap += fmt("%s|%s|%lld|%lld|%d\n", (d.serial.empty() ? d.model : d.serial).c_str(), d.model.c_str(), d.readErr, d.writeErr, d.wear);
    if (!disks.empty()) WriteText(hist + "smart_" + Stamp() + ".csv", snap, false);

    c.saveText("disk.json", rep.dump());
    if (c.res) {
        c.res->disks = disks;
        c.res->haveDisk = true;
        c.res->setSection(sec);
    }
    c.ok("Диски: готово.");
}

}  // namespace wr
