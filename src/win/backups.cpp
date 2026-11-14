#include "backups.h"
#include "../core/textutil.h"
#include <windows.h>
#include <set>
#include <mutex>

namespace wr {

namespace {
std::mutex g_mu;
std::set<std::string> g_seen;
std::vector<SessionChange> g_changes;   // журнал текущего запуска (под g_mu)
bool g_journalStarted = false;

void AppendJournal(const std::string& line) {   // UTF-8, добавление в конец
    std::string path = DataDir() + "Logs\\undo_journal.tsv";
    MakeDirs(DataDir() + "Logs\\");
    HANDLE h = CreateFileW(W(path).c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w;
    WriteFile(h, line.data(), (DWORD)line.size(), &w, nullptr);
    CloseHandle(h);
}

std::string ReadFileUtf8(const std::string& path) {
    HANDLE h = CreateFileW(W(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return "";
    std::string s;
    char buf[65536];
    DWORD r;
    while (ReadFile(h, buf, sizeof buf, &r, nullptr) && r) s.append(buf, r);
    CloseHandle(h);
    return s;
}

std::string Root() { return DataDir() + "Backups\\"; }

void Measure(const std::string& dir, unsigned long long& bytes, int& files) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(W(dir + "*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::string n = U(fd.cFileName);
        if (n == "." || n == "..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) Measure(dir + n + "\\", bytes, files);
        } else { bytes += ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow; ++files; }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}
}  // namespace

std::string VolumeId(const std::string& anyPath) {
    wchar_t vol[MAX_PATH] = {0};
    std::wstring p = W(anyPath.empty() ? std::string("C:\\") : anyPath);
    if (anyPath.empty()) { wchar_t sd[MAX_PATH]; if (GetSystemDirectoryW(sd, MAX_PATH) >= 3) p = std::wstring(sd, 3); }
    if (!GetVolumePathNameW(p.c_str(), vol, MAX_PATH)) return "vol_unknown";
    DWORD serial = 0;
    if (!GetVolumeInformationW(vol, nullptr, 0, &serial, nullptr, nullptr, nullptr, 0)) return "vol_unknown";
    return fmt("vol_%08X", (unsigned)serial);
}

std::string BackupDirFor(const std::string& anyPath, const char* kind) {
    std::string base = Root() + VolumeId(anyPath) + "\\";
    std::string dir = base + kind + "\\";
    MakeDirs(dir);
    std::string info = base + "info.txt";
    if (GetFileAttributesW(W(info).c_str()) == INVALID_FILE_ATTRIBUTES) {
        wchar_t vol[MAX_PATH] = {0}, label[MAX_PATH] = {0};
        std::wstring p = W(anyPath.empty() ? std::string("C:\\") : anyPath);
        GetVolumePathNameW(p.c_str(), vol, MAX_PATH);
        GetVolumeInformationW(vol, label, MAX_PATH, nullptr, nullptr, nullptr, nullptr, 0);
        WriteText(info, "Резервные копии WinRepair для тома " + U(vol) + " (метка: " + U(label) + ")\r\nСоздано: " + NiceNow() +
                        "\r\nПолитика: одна копия на том; обновляется один раз за сеанс перед первым изменением. Удалить можно в программе («Резервные копии…») или вручную.\r\n");
    }
    return dir;
}

std::string UndoJournalPath() { return DataDir() + "Logs\\undo_journal.tsv"; }

void RecordSessionChange(const std::string& kind, const std::string& what, const std::string& src,
                         const std::string& backup, bool online) {
    SessionChange c;
    c.time = NiceNow(); c.kind = kind; c.what = what; c.src = src; c.backup = backup; c.online = online;
    std::lock_guard<std::mutex> g(g_mu);
    g_changes.push_back(c);
    std::string hdr;
    if (!g_journalStarted) { g_journalStarted = true; hdr = "#SESSION\t" + c.time + "\r\n"; }
    AppendJournal(hdr + kind + "\t" + (online ? "1" : "0") + "\t" + what + "\t" + src + "\t" + backup + "\t" + c.time + "\r\n");
}

std::vector<SessionChange> SessionChanges() {
    std::lock_guard<std::mutex> g(g_mu);
    return g_changes;
}

std::vector<SessionChange> LoadPreviousSession(std::string& startedAt) {
    std::vector<SessionChange> out;
    startedAt.clear();
    std::string text = ReadFileUtf8(UndoJournalPath());
    size_t p = 0;
    std::vector<SessionChange> cur;
    std::string curStart;
    while (p < text.size()) {   // последний блок #SESSION, в котором есть записи
        size_t e = text.find('\n', p);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(p, e - p);
        p = e + 1;
        while (!line.empty() && (line.back() == '\r')) line.pop_back();
        if (line.empty()) continue;
        if (line.compare(0, 9, "#SESSION\t") == 0) {
            if (!cur.empty()) { out = cur; startedAt = curStart; }
            cur.clear();
            curStart = line.substr(9);
            continue;
        }
        std::vector<std::string> f;
        size_t q = 0;
        while (f.size() < 6) {
            size_t t = line.find('\t', q);
            if (t == std::string::npos) { f.push_back(line.substr(q)); break; }
            f.push_back(line.substr(q, t - q));
            q = t + 1;
        }
        if (f.size() < 6) continue;
        SessionChange c;
        c.kind = f[0]; c.online = f[1] == "1"; c.what = f[2]; c.src = f[3]; c.backup = f[4]; c.time = f[5];
        if (!c.backup.empty() && GetFileAttributesW(W(c.backup).c_str()) == INVALID_FILE_ATTRIBUTES) continue;   // копии уже нет
        cur.push_back(c);
    }
    if (!cur.empty()) { out = cur; startedAt = curStart; }
    return out;
}

bool BackupOncePerSession(const std::string& key) {
    std::lock_guard<std::mutex> g(g_mu);
    return g_seen.insert(key).second;
}

bool BackupRegText(const std::string& volumePath, const std::string& tag, const std::string& regText, std::string& pathOut) {
    std::string f = BackupDirFor(volumePath, "hives") + tag + ".reg";
    pathOut = f;
    if (!BackupOncePerSession("reg:" + f)) return false;   // already saved in this session: that copy is the pre-change state
    if (!WriteText(f, regText)) return false;
    RecordSessionChange("reg", "Ветка реестра: " + tag, volumePath, f, VolumeId(volumePath) == VolumeId(""));
    return true;
}

std::vector<BackupItem> ListBackups() {
    std::vector<BackupItem> v;
    std::string root = Root();
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(W(root + "*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return v;
    do {
        std::string n = U(fd.cFileName);
        if (n == "." || n == "..") continue;
        BackupItem it;
        it.name = n; it.path = root + n;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            Measure(it.path + "\\", it.bytes, it.files);
            it.legacy = n.compare(0, 4, "vol_") != 0;
        } else {
            it.bytes = ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow; it.files = 1; it.legacy = true;
        }
        v.push_back(it);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return v;
}

unsigned long long BackupsTotalBytes(const std::vector<BackupItem>& v) { unsigned long long t = 0; for (auto& i : v) t += i.bytes; return t; }

std::string BackupsSummary(const std::vector<BackupItem>& v) {
    std::string s = fmt("Папка: %s\nЭлементов: %zu, общий размер: %s\n", Root().c_str(), v.size(), fmtBytes(BackupsTotalBytes(v)).c_str());
    int shown = 0;
    for (auto& i : v) {
        if (++shown > 8) { s += "…\n"; break; }
        s += fmt("  • %s — %s%s\n", i.name.c_str(), fmtBytes(i.bytes).c_str(), i.legacy ? " (старый формат)" : "");
    }
    return s;
}

bool DeleteTree(const std::string& path) {
    DWORD a = GetFileAttributesW(W(path).c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return true;
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) {
        SetFileAttributesW(W(path).c_str(), FILE_ATTRIBUTE_NORMAL);
        return DeleteFileW(W(path).c_str()) != 0;
    }
    if (a & FILE_ATTRIBUTE_REPARSE_POINT) return RemoveDirectoryW(W(path).c_str()) != 0;   // never follow links
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(W(path + "\\*").c_str(), &fd);
    bool ok = true;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::string n = U(fd.cFileName);
            if (n == "." || n == "..") continue;
            if (!DeleteTree(path + "\\" + n)) ok = false;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    SetFileAttributesW(W(path).c_str(), FILE_ATTRIBUTE_NORMAL);
    return RemoveDirectoryW(W(path).c_str()) != 0 && ok;
}

int DeleteBackups(const std::vector<BackupItem>& which) {
    int n = 0;
    std::string root = Root();
    for (auto& i : which) {
        if (i.path.compare(0, root.size(), root) != 0 || i.name.empty() || i.name.find("..") != std::string::npos) continue;   // only inside Data\Backups
        if (DeleteTree(i.path)) { Logger::I().log(Lvl::Warn, "Удалена резервная копия: " + i.path); ++n; }
        else Logger::I().log(Lvl::Error, "Не удалось удалить: " + i.path);
    }
    return n;
}

}  // namespace wr
