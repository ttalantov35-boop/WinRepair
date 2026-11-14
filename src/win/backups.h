// Backup policy: ONE copy per examined Windows volume, refreshed at most once per session, listed/cleaned by the user.
//   Data\Backups\<vol_XXXXXXXX>\{hives,files,bcd}\   (XXXXXXXX = volume serial of the examined Windows volume)
// A copy is taken the first time something is about to be changed in a session (= the state before this session's edits)
// and replaces the copy of the previous session, so the folder does not grow. Nothing is deleted without the user's decision.
#pragma once
#include <string>
#include <vector>
#include "common.h"

namespace wr {

struct BackupItem {
    std::string name;      // folder (vol_XXXXXXXX) or legacy file/folder name
    std::string path;
    unsigned long long bytes = 0;
    int files = 0;
    bool legacy = false;   // old-format entry (timestamped folder / loose file)
};

std::string VolumeId(const std::string& anyPath);                 // "vol_1A2B3C4D" (or "vol_unknown")
std::string BackupDirFor(const std::string& anyPath, const char* kind);   // creates Backups\<id>\<kind>\ (+ info.txt), returns it with trailing '\'
bool BackupOncePerSession(const std::string& key);                 // true the first time `key` is seen in this run
bool BackupRegText(const std::string& volumePath, const std::string& tag, const std::string& regText, std::string& pathOut);  // .reg export, first time per session only

// ---- журнал изменений сеанса (кнопка «Отменить изменения сеанса…») -----------------------------------------
// Каждая резервная копия/перенос в карантин записывается сюда: вместе они описывают, что менялось в сеансе,
// и как вернуть состояние на его начало. Журнал дублируется в Data\Logs\undo_journal.tsv (переживает перезапуск).
struct SessionChange {
    std::string time;    // NiceNow()
    std::string kind;    // "hive" | "reg" | "bcd-live" | "file" | "quarantine"
    std::string what;    // короткое описание для человека
    std::string src;     // исходный путь (куст, файл, папка); пусто для живого хранилища BCD
    std::string backup;  // путь копии или карантина
    bool online = false; // "reg": ветка работающей Windows (возврат = reg import)
};
void RecordSessionChange(const std::string& kind, const std::string& what, const std::string& src,
                         const std::string& backup, bool online = false);
std::vector<SessionChange> SessionChanges();                       // записи текущего запуска
std::vector<SessionChange> LoadPreviousSession(std::string& startedAt);  // последний сеанс из файла журнала (только записи с сохранившимися копиями)
std::string UndoJournalPath();

std::vector<BackupItem> ListBackups();
unsigned long long BackupsTotalBytes(const std::vector<BackupItem>& v);
std::string BackupsSummary(const std::vector<BackupItem>& v);      // multi-line text for dialogs
int DeleteBackups(const std::vector<BackupItem>& which);           // returns number of removed items
bool DeleteTree(const std::string& path);                          // recursive, also removes read-only files

}  // namespace wr
