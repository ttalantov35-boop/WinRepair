// WinRE integration: discovery, one-time boot into WinRE, custom "WinRE + WinRepair" image (copy of winre.wim + our tool).
#pragma once
#include "bcd.h"

namespace wr {

struct WinReInfo {
    bool xmlFound = false;
    std::string xmlPath, installState, location, bcdId;
    std::string wimPath;                    // Winre.wim actually found (drive letter path)
    std::vector<std::string> wimCandidates;
    unsigned long long wimSize = 0;
};

WinReInfo ReadWinReInfo(const Target& t);
std::string FindWinReEntry(const std::vector<BcdEntry>& entries, const std::string& preferId, bool custom);
std::string WinReSdiFor(const std::string& wimPath, const Target& t);  // path of boot.sdi next to wim (copied from Windows\Boot if missing)
bool CreateRamdiskEntry(Ctx& c, const std::string& store, const std::string& desc, const std::string& wimPath, const std::string& sdiPath, std::string& outId);
void JobBuildWinRepairRE(Ctx& c, const std::string& store, const std::string& srcWim);
void JobRemoveWinRepairRE(Ctx& c, const std::string& store);
// свой загрузочный WinPE; baseMode: 0 — winre.wim (со встроенным rstrui), 1 — ADK WinPE, 2 — установочный образ Windows (mediaSrc: .iso / диск / папка).
// isoToDismount — образ ISO, который окно сборки уже подключило заранее: после сборки (и при ошибке) он отключается.
void JobBuildWinPE(Ctx& c, const std::string& destRoot, int baseMode, const std::string& mediaSrc, const std::string& isoToDismount = "");
// Подключение образа ISO (PowerShell Mount-DiskImage) с ожиданием буквы диска — Windows присваивает её не сразу.
struct IsoMountResult {
    bool ok = false;          // образ подключён и буква известна
    bool attachedByUs = false;// подключили мы (не был подключён раньше) — значит, нам и отключать
    char letter = 0;          // буква диска образа
    std::string error;        // причина неудачи (для журнала/окна)
};
IsoMountResult MountIsoImage(const std::string& isoPath, int waitSec = 30);
bool DismountIsoImage(const std::string& isoPath);
bool WinPeAdkInstalled();   // установлен ли ADK с дополнением WinPE
// Носитель для WinPE: что за место выбрано (корень флешки / папка / диск с Windows) — для подсказки в окне сборки.
struct BootMediaInfo {
    bool isRoot = false, exists = false, usb = false, removable = false, systemDisk = false, gpt = false, active = false, hasWinPe = false;
    std::string fs;               // файловая система (FAT32 / NTFS / exFAT ...)
    unsigned long long size = 0;  // размер тома, байт
};
BootMediaInfo QueryBootMedia(const std::string& path);
void JobPrepareBiosBoot(Ctx& c, const std::string& destRoot);   // флешка, на которую WinPE скопирован вручную: активный раздел + bootsect /nt60
void JobBootOnce(Ctx& c, const std::string& store, const std::string& id, bool reboot);
std::string RebootCommand();
bool CopyTree(const std::string& src, const std::string& dst, int& files, const std::atomic<bool>* cancel);

}  // namespace wr
