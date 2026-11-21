// Менеджер разделов: обёртка над diskpart. Листинг — через Windows API (надёжно, не зависит от языка
// diskpart), изменения — скриптами diskpart. ВСЕ операции необратимы (данные не восстановить).
// Рассчитан на работу из WinPE/WinRE над физическими дисками.
#pragma once
#include "../win/app.h"

namespace wr {

// Раздел или свободная область на диске.
struct PartItem {
    bool free = false;               // true — незанятая область (не раздел)
    unsigned number = 0;             // номер раздела для diskpart (select partition N); 0 для free
    unsigned long long offset = 0;   // смещение от начала диска, байт
    unsigned long long size = 0;     // размер, байт
    std::string type;                // тип раздела (EFI, MSR, основной, восстановление…)
    bool active = false;             // MBR: активный (загрузочный)
    char letter = 0;                 // буква тома (0 — нет)
    std::string fs;                  // файловая система тома
    std::string label;               // метка тома
    unsigned long long freeBytes = 0;// свободно на томе
    bool hasVol = false;             // есть сопоставленный том (буква/ФС)
};

struct PartDisk {
    int number = -1;
    std::string model, serial, bus, media, style;  // style: GPT / MBR / RAW
    unsigned long long size = 0;
    unsigned long long freeMax = 0;                 // крупнейшая незанятая область
    std::vector<PartItem> items;                    // разделы и свободные области по возрастанию смещения
};

// Прочитать все физические диски с разделами/томами (только чтение, Windows API).
bool PartScan(std::vector<PartDisk>& out, std::string& err);

// Операции (скрипты diskpart). Запускаются заданием, пишут в журнал. Необратимы.
void PartCreate(Ctx& c, int disk, unsigned long long sizeMB, const std::string& fs, const std::string& label, char letter);  // sizeMB 0 = всё свободное; letter 0 = не назначать
void PartDelete(Ctx& c, int disk, unsigned part);
void PartFormat(Ctx& c, int disk, unsigned part, const std::string& fs, const std::string& label, bool quick);
void PartSetLetter(Ctx& c, int disk, unsigned part, char letter);   // letter 0 = убрать букву
void PartActive(Ctx& c, int disk, unsigned part, bool active);      // только MBR
void PartShrink(Ctx& c, int disk, unsigned part, unsigned long long mb);
void PartExtend(Ctx& c, int disk, unsigned part, unsigned long long mb);  // mb 0 = максимум
void PartClean(Ctx& c, int disk);                                  // стереть таблицу разделов диска
void PartConvert(Ctx& c, int disk, bool toGpt);                    // диск должен быть пустым (после clean)

}  // namespace wr
